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
};

Pool &pool();
thread_local int tl_worker = -1;

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
// each time so workers do not all converge on the same victim.
Task *find_task(int me) {
  Pool &p = pool();
  if (me >= 0) {
    if (Task *task = take_task(*p.workers[me])) return task;
  }
  size_t count = p.workers.size();
  static thread_local unsigned rotation = 0;
  for (size_t i = 0; i < count; i++) {
    size_t victim = (rotation + i) % count;
    if ((int)victim == me) continue;
    if (Task *task = steal_task(*p.workers[victim])) {
      rotation = (unsigned)victim;
      return task;
    }
  }
  return nullptr;
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

int thread_count() {
  if (const char *env = getenv("SWORD_THREADS")) {
    int n = atoi(env);
    if (n > 0) return n;
  }
  unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : (int)n;
}

void stop_pool();

Pool &pool() {
  static Pool *instance = [] {
    Pool *p = new Pool();
    int n = thread_count();
    for (int i = 0; i < n; i++) p->workers.push_back(new Worker());
    for (int i = 1; i < n; i++) p->threads.emplace_back(worker_loop, i);
    tl_worker = 0; // the thread that starts the pool owns queue 0
    atexit(stop_pool);
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
  p.wake.notify_one();
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
