#include "sword_rt.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Arguments up to this size ride along inside the task; bigger ones get their
// own allocation. Spawning in a loop is the common case, so the fast path has
// to avoid the allocator.
const int64_t kInlineArgs = 96;

// A hired thread waits this many idle milliseconds before retiring. Long
// enough that a burst of blocking calls reuses the same threads, short enough
// that a server which went quiet gives them back.
const int kHelperIdleMillis = 200;

// How far the pool may grow past the fixed workers when threads park in
// syscalls. Threads are cheap next to a socket but not free.
const int64_t kDefaultCeiling = 512;

struct Scope {
  std::atomic<int64_t> outstanding;
  std::atomic<unsigned> failed; // holds the first error code seen
};

static_assert(sizeof(Scope) <= SWORD_SCOPE_SIZE, "scope blob too small");
// Generated code hands us a word-aligned slot; anything stricter would
// need the compiler to know about it.
static_assert(alignof(Scope) <= 8, "scope needs more alignment than the"
                                   " caller's slot provides");

struct Task {
  sword_task_fn fn;
  Scope *scope;
  void *heap_args;
  alignas(16) unsigned char args[kInlineArgs];
};

struct Worker {
  std::mutex lock;
  std::vector<Task *> queue; // back is the owner's end, front is stolen from
  std::vector<Task *> spare;
};

struct Pool {
  std::vector<Worker *> workers;
  std::vector<std::thread> threads;
  std::mutex sleep_lock;
  std::condition_variable wake;
  std::atomic<bool> stopping{false};
  std::atomic<int64_t> ready{0};

  int64_t target = 1;   // fixed workers, one per core unless told otherwise
  int64_t ceiling = 1;  // fixed workers plus the most hires allowed
  std::atomic<int64_t> pending{0}; // spawned and not yet picked up
  std::atomic<int64_t> parked{0};  // threads sitting in a syscall
  std::atomic<int64_t> hired{0};   // extra threads covering for them
  std::mutex hire_lock;
};

Pool &pool();
// Null until the pool is up, which is how the blocking hints stay free for a
// program that never spawns a task.
std::atomic<Pool *> running{nullptr};
thread_local int tl_worker = -1;
// Blocking hints this thread has counted, so an unmatched exit cannot push the
// pool's tally negative.
thread_local int tl_parked = 0;

Task *take_task(Worker &w) {
  std::lock_guard<std::mutex> held(w.lock);
  if (w.queue.empty()) return nullptr;
  Task *task = w.queue.back();
  w.queue.pop_back();
  return task;
}

Task *steal_task(Worker &w) {
  std::lock_guard<std::mutex> held(w.lock);
  if (w.queue.empty()) return nullptr;
  Task *task = w.queue.front();
  w.queue.erase(w.queue.begin());
  return task;
}

// Own queue first, then everyone else's, starting from a different neighbour
// each time so workers do not all converge on the same victim. A hired thread
// passes -1: it has no queue of its own and only steals.
Task *find_task(int me) {
  Pool &p = pool();
  Task *task = me >= 0 ? take_task(*p.workers[me]) : nullptr;
  if (!task) {
    size_t count = p.workers.size();
    static thread_local unsigned rotation = 0;
    for (size_t i = 0; i < count && !task; i++) {
      size_t victim = (rotation + i) % count;
      if ((int)victim == me) continue;
      task = steal_task(*p.workers[victim]);
      if (task) rotation = (unsigned)victim;
    }
  }
  if (task) p.pending.fetch_sub(1, std::memory_order_relaxed);
  return task;
}

void recycle(Task *task) {
  if (task->heap_args) {
    free(task->heap_args);
    task->heap_args = nullptr;
  }
  Pool &p = pool();
  int me = tl_worker >= 0 ? tl_worker : 0;
  Worker &w = *p.workers[me];
  std::lock_guard<std::mutex> held(w.lock);
  if (w.spare.size() < 256) w.spare.push_back(task);
  else delete task;
}

void run_task(Task *task) {
  void *args = task->heap_args ? task->heap_args : (void *)task->args;
  uint16_t code = task->fn(args);
  Scope *scope = task->scope;
  if (code != 0) {
    unsigned none = 0;
    scope->failed.compare_exchange_strong(none, code);
  }
  recycle(task);
  scope->outstanding.fetch_sub(1, std::memory_order_release);
}

void worker_loop(int me) {
  tl_worker = me;
  Pool &p = pool();
  while (!p.stopping.load(std::memory_order_acquire)) {
    if (Task *task = find_task(me)) {
      run_task(task);
      continue;
    }
    std::unique_lock<std::mutex> held(p.sleep_lock);
    if (p.stopping.load(std::memory_order_acquire)) break;
    p.wake.wait_for(held, std::chrono::milliseconds(1));
  }
}

// A thread hired to cover for one that parked in a syscall. It steals, never
// owns a queue, and retires once the work it was hired for has dried up.
void helper_loop() {
  tl_worker = -1;
  Pool &p = pool();
  int idle = 0;
  while (!p.stopping.load(std::memory_order_acquire)) {
    if (Task *task = find_task(-1)) {
      run_task(task);
      idle = 0;
      continue;
    }
    if (++idle > kHelperIdleMillis) break;
    std::unique_lock<std::mutex> held(p.sleep_lock);
    if (p.stopping.load(std::memory_order_acquire)) break;
    p.wake.wait_for(held, std::chrono::milliseconds(1));
  }
  p.hired.fetch_sub(1, std::memory_order_release);
}

// Capacity is every thread that is not parked in a syscall. When that falls
// below the target and tasks are queued behind it, hire one more.
void cover_for_parked(Pool &p) {
  // Both loads are relaxed on purpose: this is a heuristic, and the next
  // blocking call corrects an answer that was stale by a hair.
  if (p.parked.load(std::memory_order_relaxed) == 0) return;
  if (p.pending.load(std::memory_order_relaxed) <= 0) return;

  std::lock_guard<std::mutex> held(p.hire_lock);
  int64_t live = p.target + p.hired.load(std::memory_order_acquire);
  int64_t runnable = live - p.parked.load(std::memory_order_acquire);
  if (runnable >= p.target) return;
  // The ceiling is a brake, not a wall. Holding to it while nothing at all can
  // run would turn a program that is merely over its thread budget into one
  // that has stopped, so the last thread is always allowed through.
  if (live >= p.ceiling && runnable > 0) return;
  p.hired.fetch_add(1, std::memory_order_release);
  // Detached: a helper only touches the pool, which outlives the process.
  std::thread(helper_loop).detach();
}

int64_t env_count(const char *name, int64_t fallback) {
  if (const char *text = getenv(name)) {
    int n = atoi(text);
    if (n > 0) return n;
  }
  return fallback;
}

int thread_count() {
  unsigned n = std::thread::hardware_concurrency();
  return (int)env_count("SWORD_THREADS", n == 0 ? 1 : (int64_t)n);
}

void stop_pool();

Pool &pool() {
  static Pool *instance = [] {
    Pool *p = new Pool();
    int n = thread_count();
    p->target = n;
    p->ceiling = env_count("SWORD_MAX_THREADS", kDefaultCeiling);
    if (p->ceiling < p->target) p->ceiling = p->target;
    for (int i = 0; i < n; i++) p->workers.push_back(new Worker());
    for (int i = 1; i < n; i++) p->threads.emplace_back(worker_loop, i);
    tl_worker = 0; // the thread that starts the pool owns queue 0
    atexit(stop_pool);
    running.store(p, std::memory_order_release);
    return p;
  }();
  return *instance;
}

void stop_pool() {
  Pool &p = pool();
  p.stopping.store(true, std::memory_order_release);
  p.wake.notify_all();
  for (std::thread &t : p.threads)
    if (t.joinable()) t.join();
  // Hired threads are detached, so give them a moment to notice. One that is
  // still parked in a syscall stays there; everything it can reach outlives
  // the process anyway.
  for (int i = 0; i < 100 && p.hired.load(std::memory_order_acquire) > 0; i++) {
    p.wake.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Task *fresh_task() {
  Pool &p = pool();
  int me = tl_worker >= 0 ? tl_worker : 0;
  Worker &w = *p.workers[me];
  {
    std::lock_guard<std::mutex> held(w.lock);
    if (!w.spare.empty()) {
      Task *task = w.spare.back();
      w.spare.pop_back();
      return task;
    }
  }
  return new Task();
}

// Instead of idling, a thread waiting on a scope runs whatever work it can
// find. That is what makes nested scopes safe from deadlock.
void drain_until(Scope *scope) {
  while (scope->outstanding.load(std::memory_order_acquire) > 0) {
    if (Task *task = find_task(tl_worker)) run_task(task);
    else std::this_thread::yield();
  }
}

} // namespace

extern "C" {

void sword_scope_begin(void *blob) {
  Scope *scope = new (blob) Scope();
  scope->outstanding.store(0, std::memory_order_relaxed);
  scope->failed.store(0, std::memory_order_relaxed);
  pool();
}

void sword_scope_spawn(void *blob, sword_task_fn fn, const void *args,
                       int64_t size) {
  Scope *scope = (Scope *)blob;
  Task *task = fresh_task();
  task->fn = fn;
  task->scope = scope;
  task->heap_args = nullptr;
  if (size > kInlineArgs) {
    task->heap_args = malloc((size_t)size);
    memcpy(task->heap_args, args, (size_t)size);
  } else if (size > 0) {
    memcpy(task->args, args, (size_t)size);
  }

  scope->outstanding.fetch_add(1, std::memory_order_relaxed);

  Pool &p = pool();
  Worker &w = *p.workers[tl_worker >= 0 ? tl_worker : 0];
  {
    std::lock_guard<std::mutex> held(w.lock);
    w.queue.push_back(task);
  }
  p.pending.fetch_add(1, std::memory_order_relaxed);
  p.wake.notify_one();
  // Work arriving while threads are parked is the other half of the hiring
  // rule: without this, a task spawned after everyone blocked would wait for
  // the next blocking call to notice it.
  cover_for_parked(p);
}

void sword_blocking_enter(void) {
  Pool *p = running.load(std::memory_order_acquire);
  if (!p) return;
  tl_parked++;
  p->parked.fetch_add(1, std::memory_order_acq_rel);
  cover_for_parked(*p);
}

// Counts down only what this thread counted up: the pool may well have come
// up while the thread was already inside the syscall.
void sword_blocking_exit(void) {
  if (tl_parked == 0) return;
  tl_parked--;
  running.load(std::memory_order_acquire)
      ->parked.fetch_sub(1, std::memory_order_acq_rel);
}

uint16_t sword_scope_end(void *blob) {
  Scope *scope = (Scope *)blob;
  drain_until(scope);
  return (uint16_t)scope->failed.load(std::memory_order_acquire);
}

uint16_t sword_parallel_for(int64_t lo, int64_t hi, sword_chunk_fn fn,
                            void *env) {
  if (hi <= lo) return 0;

  Pool &p = pool();
  int64_t total = hi - lo;
  // Several chunks per worker: enough slack for stealing to even out an
  // uneven body, without paying task overhead per iteration.
  int64_t want = (int64_t)p.workers.size() * 4;
  int64_t chunk = (total + want - 1) / want;
  if (chunk < 1) chunk = 1;

  struct Range {
    sword_chunk_fn fn;
    void *env;
    int64_t lo, hi;
  };

  alignas(8) unsigned char blob[SWORD_SCOPE_SIZE];
  sword_scope_begin(blob);
  for (int64_t at = lo; at < hi; at += chunk) {
    Range r{fn, env, at, at + chunk > hi ? hi : at + chunk};
    sword_scope_spawn(
        blob,
        [](void *args) -> uint16_t {
          Range *r = (Range *)args;
          return r->fn(r->env, r->lo, r->hi);
        },
        &r, (int64_t)sizeof(Range));
  }
  return sword_scope_end(blob);
}
}
