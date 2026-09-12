#include "sword_rt.h"
#include "sword_poll.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Written in assembly, one per architecture. `switch` saves the callee-saved
// registers on the stack it is leaving and resumes the one it is given;
// `start` is where a stack that has never run begins.
extern "C" {
void sword_ctx_switch(void **save_sp, void *resume_sp);
void sword_ctx_start(void);
void sword_fiber_entry(void);
}

// The sanitizers track one stack per thread, so a task that moves between
// stacks looks to them like memory appearing and disappearing. These are the
// hooks that tell them otherwise; without them ASan reports stack overflows
// that are not there and TSan reports races between two tasks that never ran
// at the same time.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SWORD_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define SWORD_TSAN 1
#endif
#endif

#ifdef SWORD_ASAN
extern "C" void __sanitizer_start_switch_fiber(void **fake, const void *bottom,
                                               size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void *fake, const void **bottom,
                                                size_t *size);
#endif
#ifdef SWORD_TSAN
extern "C" void *__tsan_get_current_fiber(void);
extern "C" void *__tsan_create_fiber(unsigned flags);
extern "C" void __tsan_destroy_fiber(void *fiber);
extern "C" void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif

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

// --- stacks --------------------------------------------------------------

// Reserved, not committed: the pages a task never touches cost nothing but
// address space, and the measured cost of an idle connection is what it touches
// rather than what it reserved. Sword code puts whole buffers on the stack — a
// connection's arena lives there — so this is roomy on purpose.
const size_t kStackReserve = 1024 * 1024;

// A task and the stack it runs on. It outlives any one worker: a task that
// stops for I/O is resumed by whichever worker picks it up next.
// Where a task is between leaving a worker and coming back. The handoff
// matters: the poller may answer before the task has finished switching out, so
// whichever of the two gets there second is the one that queues it.
enum FiberState { FIBER_RUNNING, FIBER_PARKING, FIBER_PARKED, FIBER_READY };

struct Fiber {
  void *sp = nullptr;       // where to resume this task
  void *sched_sp = nullptr; // where to go back to, set on every entry
  char *base = nullptr;     // the mapping, guard page first
  size_t size = 0;
  Task *task = nullptr;
  bool finished = false;
  std::atomic<int> state{FIBER_RUNNING};
  int wake_result = 0;
  int home = 0; // the worker it last ran on, where a wake puts it back
#ifdef SWORD_ASAN
  void *fake_stack = nullptr;
#endif
#ifdef SWORD_TSAN
  void *tsan = nullptr;
#endif
};

size_t page_size() {
  static size_t size = (size_t)sysconf(_SC_PAGESIZE);
  return size;
}

// A guard page at the low end turns a stack overflow into a fault at the point
// of overflow, rather than into a quiet write through somebody else's memory.
bool map_stack(Fiber *f) {
  size_t guard = page_size();
  size_t total = kStackReserve + guard;
  void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) return false;
  if (mprotect(base, guard, PROT_NONE) != 0) {
    munmap(base, total);
    return false;
  }
  f->base = (char *)base;
  f->size = total;
  return true;
}

// Lays out a frame the context switch can resume into: zeroed callee-saved
// registers and a return address of `sword_ctx_start`, which calls the entry
// point below.
void prepare(Fiber *f) {
  char *top = f->base + f->size;
  uintptr_t aligned = (uintptr_t)top & ~(uintptr_t)15;

#if defined(__aarch64__) || defined(__arm64__)
  // 192 bytes: x19-x28, x29, x30, d8-d15. The link register sits at 88.
  char *frame = (char *)(aligned - 192);
  memset(frame, 0, 192);
  void (*start)(void) = sword_ctx_start;
  memcpy(frame + 88, &start, sizeof(start));
  f->sp = frame;
#elif defined(__x86_64__)
  // r15, r14, r13, r12, rbx, rbp, then the address `ret` jumps to. That last
  // slot has to be 16-aligned so the entry sees the stack a call would leave.
  char *ret_slot = (char *)(aligned - 16);
  void (*start)(void) = sword_ctx_start;
  memcpy(ret_slot, &start, sizeof(start));
  memset(ret_slot - 48, 0, 48);
  f->sp = ret_slot - 48;
#else
#error "sword: no stack layout for this architecture"
#endif
  f->finished = false;
}

struct Worker {
  std::mutex lock;
  std::vector<Task *> queue; // back is the owner's end, front is stolen from
  std::vector<Fiber *> ready; // parked tasks the poller has woken
  std::vector<Task *> spare;
  std::vector<Fiber *> stacks; // mapped once, reused
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
// The task running on this thread right now, and where to go back to when it
// stops. Both are read fresh after every switch: a task may well come back on
// a different thread than it left.
thread_local Fiber *tl_fiber = nullptr;
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

// A task the poller has woken. These come before fresh work: resuming one costs
// nothing but a switch, and finishing what has been started is what keeps the
// number of live stacks down.
Fiber *find_ready(int me) {
  Pool &p = pool();
  size_t count = p.workers.size();
  for (size_t i = 0; i < count; i++) {
    size_t at = (me >= 0 ? (size_t)me + i : i) % count;
    Worker &w = *p.workers[at];
    std::lock_guard<std::mutex> held(w.lock);
    if (w.ready.empty()) continue;
    Fiber *f = w.ready.back();
    w.ready.pop_back();
    p.pending.fetch_sub(1, std::memory_order_relaxed);
    return f;
  }
  return nullptr;
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

void finish_task(Task *task, uint16_t code) {
  Scope *scope = task->scope;
  if (code != 0) {
    unsigned none = 0;
    scope->failed.compare_exchange_strong(none, code);
  }
  recycle(task);
  scope->outstanding.fetch_sub(1, std::memory_order_release);
}

Fiber *fresh_fiber(Task *task);
void retire_fiber(Fiber *f);

// Switching in. Nothing after the switch may assume it is still on the thread
// it started on, which is why the sanitizer bookkeeping brackets it here and
// nothing is cached across it.
void enter(Fiber *f) {
  tl_fiber = f;
#ifdef SWORD_TSAN
  if (!f->tsan) f->tsan = __tsan_create_fiber(0);
  void *back = __tsan_get_current_fiber();
  __tsan_switch_to_fiber(f->tsan, 0);
#endif
#ifdef SWORD_ASAN
  __sanitizer_start_switch_fiber(&f->fake_stack, f->base, f->size);
#endif
  sword_ctx_switch(&f->sched_sp, f->sp);
#ifdef SWORD_ASAN
  const void *bottom = nullptr;
  size_t size = 0;
  __sanitizer_finish_switch_fiber(f->fake_stack, &bottom, &size);
#endif
#ifdef SWORD_TSAN
  __tsan_switch_to_fiber(back, 0);
#endif
  tl_fiber = nullptr;
}

// Switching out, from inside the task. `f` comes off the task's own stack, so
// no thread-local is read on the way out.
void leave(Fiber *f, bool done) {
#ifdef SWORD_ASAN
  __sanitizer_start_switch_fiber(done ? nullptr : &f->fake_stack, nullptr, 0);
#endif
  sword_ctx_switch(&f->sp, f->sched_sp);
#ifdef SWORD_ASAN
  const void *bottom = nullptr;
  size_t size = 0;
  __sanitizer_finish_switch_fiber(f->fake_stack, &bottom, &size);
#endif
}

void make_runnable(Fiber *f) {
  Pool &p = pool();
  Worker &w = *p.workers[f->home < (int)p.workers.size() ? f->home : 0];
  {
    std::lock_guard<std::mutex> held(w.lock);
    w.ready.push_back(f);
  }
  p.pending.fetch_add(1, std::memory_order_relaxed);
  p.wake.notify_one();
}

// Called by the poller, on its own thread. The task may still be switching out
// when this runs, which is what the state machine is for.
void on_ready(void *token, int result) {
  Fiber *f = (Fiber *)token;
  f->wake_result = result;
  if (f->state.exchange(FIBER_READY, std::memory_order_acq_rel) == FIBER_PARKED)
    make_runnable(f);
}

// Back from a task: either it is done, or it has parked and the poller owns it
// now — unless the poller got there first, in which case it is runnable again
// already.
void after_enter(Fiber *f) {
  if (f->finished) {
    retire_fiber(f);
    return;
  }
  int parking = FIBER_PARKING;
  if (!f->state.compare_exchange_strong(parking, FIBER_PARKED,
                                        std::memory_order_acq_rel))
    make_runnable(f);
}

void run_task(Task *task) {
  Fiber *f = fresh_fiber(task);
  f->home = tl_worker >= 0 ? tl_worker : 0;
  enter(f);
  after_enter(f);
}

void resume_fiber(Fiber *f) {
  f->home = tl_worker >= 0 ? tl_worker : 0;
  f->state.store(FIBER_RUNNING, std::memory_order_release);
  enter(f);
  after_enter(f);
}

void worker_loop(int me) {
  tl_worker = me;
  Pool &p = pool();
  while (!p.stopping.load(std::memory_order_acquire)) {
    if (Fiber *f = find_ready(me)) {
      resume_fiber(f);
      continue;
    }
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
    if (Fiber *f = find_ready(-1)) {
      resume_fiber(f);
      idle = 0;
      continue;
    }
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
  sword_poll_stop();
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

Fiber *fresh_fiber(Task *task) {
  Pool &p = pool();
  Worker &w = *p.workers[tl_worker >= 0 ? tl_worker : 0];
  Fiber *f = nullptr;
  {
    std::lock_guard<std::mutex> held(w.lock);
    if (!w.stacks.empty()) {
      f = w.stacks.back();
      w.stacks.pop_back();
    }
  }
  if (!f) {
    f = new Fiber();
    if (!map_stack(f)) {
      fputs("sword: out of memory for a task stack\n", stderr);
      abort();
    }
  }
  f->task = task;
  prepare(f);
  return f;
}

// Stacks go back on the worker's own pile rather than to the kernel: mapping
// one is far more expensive than keeping it.
void retire_fiber(Fiber *f) {
  Pool &p = pool();
  Worker &w = *p.workers[tl_worker >= 0 ? tl_worker : 0];
  std::lock_guard<std::mutex> held(w.lock);
  if (w.stacks.size() < 64) {
    w.stacks.push_back(f);
    return;
  }
#ifdef SWORD_TSAN
  if (f->tsan) __tsan_destroy_fiber(f->tsan);
#endif
  munmap(f->base, f->size);
  delete f;
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

// --- the guard inside a shared -------------------------------------------

// Plain fields with explicit atomic builtins rather than std::atomic, because
// nothing constructs this: it comes into existence as sixteen zero bytes,
// wherever the `shared` around it happens to live.
struct Guard {
  int32_t held;
  uint64_t owner; // which thread, so locking twice is a diagnostic not a hang
};

static_assert(sizeof(Guard) <= SWORD_GUARD_SIZE, "guard blob too small");
static_assert(alignof(Guard) <= 8, "guard needs more alignment than the"
                                   " caller's slot provides");

uint64_t thread_tag() {
  static std::atomic<uint64_t> next{1};
  // Zero is "nobody", so tags start at one.
  static thread_local uint64_t mine = next.fetch_add(1);
  return mine;
}

bool take_guard(Guard *g, uint64_t me) {
  int32_t idle = 0;
  if (!__atomic_compare_exchange_n(&g->held, &idle, 1, false, __ATOMIC_ACQUIRE,
                                   __ATOMIC_RELAXED))
    return false;
  __atomic_store_n(&g->owner, me, __ATOMIC_RELEASE);
  return true;
}

} // namespace

extern "C" {

// Where a task begins. It reads the thread-local once, immediately after the
// switch that landed here, and works from its own stack afterwards.
void sword_fiber_entry(void) {
#ifdef SWORD_ASAN
  const void *bottom = nullptr;
  size_t size = 0;
  __sanitizer_finish_switch_fiber(nullptr, &bottom, &size);
#endif
  Fiber *f = tl_fiber;
  Task *task = f->task;
  void *args = task->heap_args ? task->heap_args : (void *)task->args;
  uint16_t code = task->fn(args);
  finish_task(task, code);
  f->finished = true;
  leave(f, true);
  __builtin_unreachable();
}

void sword_mutex_lock(void *blob) {
  Guard *g = (Guard *)blob;
  uint64_t me = thread_tag();

  if (__atomic_load_n(&g->held, __ATOMIC_ACQUIRE) == 1 &&
      __atomic_load_n(&g->owner, __ATOMIC_ACQUIRE) == me) {
    // The checker catches the case it can see. This is the one it cannot: two
    // `lock` blocks on the same value with a call in between.
    fputs("sword: deadlock, this task already holds this shared value\n",
          stderr);
    abort();
  }

  if (take_guard(g, me)) return;

  // Contended. Spin briefly for the usual short critical section, then start
  // sleeping — and tell the scheduler, so the thread this task is using goes
  // to other work instead of waiting here.
  sword_blocking_enter();
  int64_t nap = 1000; // nanoseconds, doubling to a millisecond
  for (int spins = 0; !take_guard(g, me); spins++) {
    if (spins < 64) {
      std::this_thread::yield();
      continue;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(nap));
    if (nap < 1000000) nap *= 2;
  }
  sword_blocking_exit();
}

// Owner first: it is what the self-deadlock check reads, and clearing it before
// releasing keeps a thread that locks the same value twice in a row from
// mistaking its own stale tag for a live one.
void sword_mutex_unlock(void *blob) {
  Guard *g = (Guard *)blob;
  __atomic_store_n(&g->owner, (uint64_t)0, __ATOMIC_RELEASE);
  __atomic_store_n(&g->held, 0, __ATOMIC_RELEASE);
}
}

namespace {

// Instead of idling, a thread waiting on a scope runs whatever work it can
// find. That is what makes nested scopes safe from deadlock.
void drain_until(Scope *scope) {
  while (scope->outstanding.load(std::memory_order_acquire) > 0) {
    if (Fiber *f = find_ready(tl_worker)) resume_fiber(f);
    else if (Task *task = find_task(tl_worker)) run_task(task);
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

// Waits for a descriptor from inside a task, without holding the thread: the
// task is put down, the worker goes to other work, and the poller picks it up
// again when the kernel says so. Returns 0 when ready, -2 when the deadline
// passed, and -1 when there is no task to put down — the caller then waits the
// old way, on the thread.
int sword_park_fd(int32_t fd, int32_t writable, int64_t deadline_ns) {
  Fiber *f = tl_fiber;
  if (!f) return SWORD_POLL_FAILED;

  sword_poll_start(on_ready);
  f->wake_result = SWORD_POLL_READY;
  f->state.store(FIBER_PARKING, std::memory_order_release);
  sword_poll_wait((int)fd, (int)writable, f, deadline_ns);
  leave(f, false);
  // Resumed, possibly on another thread. Everything from here reads fresh.
  return f->wake_result;
}

int32_t sword_in_task(void) { return tl_fiber != nullptr; }

void sword_forget_fd(int32_t fd) { sword_poll_forget((int)fd); }

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
