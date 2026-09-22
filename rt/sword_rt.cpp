#include "sword_rt.h"
#include "sword_poll.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <signal.h>
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

// Stacks are handed out from slabs: one mapping holds many of them, which is
// one syscall per slab instead of one per task, and — because a slab is
// contiguous and every stack in it is the same size — turns a fault address
// into "which stack, how far down" with arithmetic alone. That is what lets an
// overflow be reported from inside a signal handler, where a lock would be a
// deadlock waiting to happen.
const size_t kStacksPerSlab = 32;
// Enough slabs for sixty-five thousand live tasks. Past that the kernel's limit
// on mappings arrives first, and no arrangement of ours moves it.
const int kMaxSlabs = 2048;

// Wider than a page on purpose. A single guard page catches a function that
// walks down its frame, but not one that reserves a large buffer and writes
// into the middle of it — that write can step clean over the guard, and on the
// other side of it is another task's stack.
const size_t kGuardBytes = 64 * 1024;

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
  Fiber *waiting_next = nullptr; // next on a shared value's wait list
  int home = 0; // the worker it last ran on, where a wake puts it back
#ifdef SWORD_ASAN
  // The sanitizer's token for this stack while it is put down, and where it
  // came from — which is what has to be handed back on the way out.
  void *fake_stack = nullptr;
  const void *from_bottom = nullptr;
  size_t from_size = 0;
#endif
#ifdef SWORD_TSAN
  void *tsan = nullptr;
#endif
};

size_t page_size() {
  static size_t size = (size_t)sysconf(_SC_PAGESIZE);
  return size;
}

size_t round_up(size_t bytes, size_t to) { return (bytes + to - 1) / to * to; }

// Every slab ever mapped, and the stacks in them that nobody is using. The
// bases are published for the fault handler to read, which is why they are
// atomic and why nothing is ever taken out of the array: a slab lives as long
// as the process, so an address in one can be recognised without the lock.
struct Slabs {
  std::mutex lock;
  std::vector<char *> spare; // the low end of each free stack's guard
  std::atomic<char *> base[kMaxSlabs];
  std::atomic<int> count{0};
  std::atomic<size_t> stride{0}; // guard plus stack, set before the first slab
  size_t guard = 0;
  size_t stack = 0;
};

Slabs g_slabs; // not a local static: the fault handler cannot run an initialiser

int64_t env_count(const char *name, int64_t fallback);

// How much stack a task gets. Held to a page multiple and to something a
// 64-bit address space can afford a great many of.
size_t stack_bytes() {
  int64_t kb = env_count("SWORD_STACK_KB", (int64_t)(kStackReserve / 1024));
  size_t want = (size_t)kb * 1024;
  if (want < 64 * 1024) want = 64 * 1024;
  if (want > 256u * 1024 * 1024) want = 256u * 1024 * 1024;
  return round_up(want, page_size());
}

// Carves one mapping into stacks, each with its guard below it. Called with the
// lock held and only when nothing is spare.
bool add_slab(Slabs &s) {
  if (s.stride.load(std::memory_order_relaxed) == 0) {
    s.guard = round_up(kGuardBytes, page_size());
    s.stack = stack_bytes();
    s.stride.store(s.guard + s.stack, std::memory_order_release);
  }
  int at = s.count.load(std::memory_order_relaxed);
  if (at >= kMaxSlabs) return false;
  size_t stride = s.stride.load(std::memory_order_relaxed);
  size_t total = stride * kStacksPerSlab;
  char *base = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) return false;
  for (size_t i = 0; i < kStacksPerSlab; i++) {
    if (mprotect(base + i * stride, s.guard, PROT_NONE) == 0) continue;
    munmap(base, total);
    return false;
  }
  for (size_t i = 0; i < kStacksPerSlab; i++)
    s.spare.push_back(base + i * stride);
  // Last, so the handler never sees a slab whose guards are still writable.
  s.base[at].store(base, std::memory_order_release);
  s.count.store(at + 1, std::memory_order_release);
  return true;
}

bool map_stack(Fiber *f) {
  Slabs &s = g_slabs;
  std::lock_guard<std::mutex> held(s.lock);
  if (s.spare.empty() && !add_slab(s)) return false;
  f->base = s.spare.back();
  f->size = s.stride.load(std::memory_order_relaxed);
  s.spare.pop_back();
  return true;
}

// A slab is never given back, so the way to stop paying for a spike of ten
// thousand connections is to drop the pages the stacks touched. The next task
// to land here gets them back zeroed, which a stack about to be written over
// does not mind.
void unmap_stack(Fiber *f) {
  Slabs &s = g_slabs;
  char *stack = f->base + s.guard;
#ifdef MADV_FREE
  madvise(stack, s.stack, MADV_FREE);
#else
  madvise(stack, s.stack, MADV_DONTNEED);
#endif
  std::lock_guard<std::mutex> held(s.lock);
  s.spare.push_back(f->base);
}

// --- running out of stack ------------------------------------------------

// Without this a task that runs off the bottom of its stack takes the process
// with it and leaves behind a fault address, which says nothing about what
// happened. The handler cannot run on the stack that overflowed, so every
// thread that runs tasks gets a small one of its own for it.
struct sigaction g_was_segv;
struct sigaction g_was_bus;

// Only reads published atomics and writes to a descriptor, which is all a
// handler may safely do.
bool fault_in_guard(const void *addr) {
  size_t stride = g_slabs.stride.load(std::memory_order_acquire);
  if (stride == 0) return false;
  const char *at = (const char *)addr;
  int slabs = g_slabs.count.load(std::memory_order_acquire);
  for (int i = 0; i < slabs; i++) {
    const char *base = g_slabs.base[i].load(std::memory_order_acquire);
    if (!base || at < base || at >= base + stride * kStacksPerSlab) continue;
    return (size_t)(at - base) % stride < g_slabs.guard;
  }
  return false;
}

// Digits by hand: a handler may not call into stdio.
char *put_number(char *out, size_t value) {
  char digits[24];
  int n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value > 0);
  while (n > 0) *out++ = digits[--n];
  return out;
}

void on_fault(int sig, siginfo_t *info, void *ctx) {
  if (fault_in_guard(info->si_addr)) {
    char text[128];
    char *end = text;
    const char *head = "sword: a task ran out of stack. It had ";
    while (*head) *end++ = *head++;
    end = put_number(end, g_slabs.stack / 1024);
    const char *tail = " KiB; SWORD_STACK_KB sets that.\n";
    while (*tail) *end++ = *tail++;
    ssize_t wrote = write(2, text, (size_t)(end - text));
    (void)wrote;
    _exit(134); // what a shell reports for a process killed by abort
  }
  // Somebody else's fault, so it goes to whoever was handling it before us —
  // a sanitizer, usually, and its report is better than anything we could say.
  // Handing it on rather than putting it back matters: `sigaction` is for the
  // whole process, so restoring it would leave every later overflow, on every
  // thread, with no explanation because one wild pointer went past once.
  const struct sigaction &prev = sig == SIGBUS ? g_was_bus : g_was_segv;
  if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
    prev.sa_sigaction(sig, info, ctx);
    return;
  }
  if (prev.sa_handler == SIG_IGN) return;
  if (prev.sa_handler && prev.sa_handler != SIG_DFL) {
    prev.sa_handler(sig);
    return;
  }
  // Nobody was handling it: the default action is to die, and the way to get it
  // is to let the instruction run again with the default back in place.
  sigaction(sig, &prev, nullptr);
}

// Where the handler runs, one per thread, from mmap rather than the allocator: a
// signal stack should not depend on the heap, least of all when what went wrong
// might be the heap. Given back when the thread ends, which is the last moment
// it could fault.
struct AltStack {
  void *base = nullptr;
  size_t size = 0;

  ~AltStack() {
    if (!base) return;
    stack_t off;
    memset(&off, 0, sizeof(off));
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, nullptr);
    munmap(base, size);
  }
};

thread_local AltStack tl_alt;

// Called by every thread that may run a task, before it runs one.
void watch_for_overflow() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_was_segv);
    sigaction(SIGBUS, &sa, &g_was_bus);
  });
  if (tl_alt.base) return;
  // A thread may already have one, and then it belongs to somebody else: ASan
  // gives each of its threads an alternate stack and, when the thread ends, takes
  // back whatever it finds registered. Replacing that registration means ASan
  // unmaps memory of ours and leaves its own mapped but forgotten — and the
  // address it forgot goes back into its allocator, which then writes into a page
  // that is no longer there. It cost an afternoon: a sanitized run died inside the
  // allocator, on a thread the sanitizer no longer recognised, with no stack to
  // show for it. Our handler needs a few hundred bytes, so anybody else's will do.
  stack_t have;
  memset(&have, 0, sizeof(have));
  if (sigaltstack(nullptr, &have) == 0 && have.ss_sp &&
      !(have.ss_flags & SS_DISABLE))
    return;
  // The handler writes one short line and leaves, so this is roomy already.
  size_t want = MINSIGSTKSZ > 32 * 1024 ? (size_t)MINSIGSTKSZ : 32 * 1024;
  size_t size = round_up(want, page_size());
  void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return; // no room to report, so report nothing
  tl_alt.base = mem;
  tl_alt.size = size;
  stack_t s;
  memset(&s, 0, sizeof(s));
  s.ss_sp = mem;
  s.ss_size = size;
  sigaltstack(&s, nullptr);
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
#ifdef SWORD_ASAN
  // A stack that has never run has no token to restore.
  f->fake_stack = nullptr;
#endif
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
  // Counted for the sake of whoever has to run this in production.
  std::atomic<int64_t> stacks{0};
  std::atomic<int64_t> started{0};
  std::atomic<int64_t> finished{0};
  std::mutex hire_lock;
  // The workers past the one that started the pool are not there from the
  // beginning: `main` is a task, so every program has a pool, and a program
  // that never spawns has exactly one thing to run.
  std::atomic<bool> workers_up{false};
  std::mutex start_lock;
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
  // The token for the worker's own stack, so it can be restored when the task
  // hands control back. The bottom and size describe where we are going.
  void *worker_fake = nullptr;
  __sanitizer_start_switch_fiber(&worker_fake, f->base, f->size);
#endif
  sword_ctx_switch(&f->sched_sp, f->sp);
#ifdef SWORD_ASAN
  __sanitizer_finish_switch_fiber(worker_fake, nullptr, nullptr);
#endif
#ifdef SWORD_TSAN
  __tsan_switch_to_fiber(back, 0);
#endif
  tl_fiber = nullptr;
}

// Called on the task's own stack, right after arriving on it. Records where it
// came from, which is what `leave` has to describe on the way back.
void arrived(Fiber *f) {
#ifdef SWORD_ASAN
  __sanitizer_finish_switch_fiber(f->fake_stack, &f->from_bottom,
                                  &f->from_size);
#else
  (void)f;
#endif
}

// Switching out, from inside the task. `f` comes off the task's own stack, so
// no thread-local is read on the way out.
void leave(Fiber *f, bool done) {
#ifdef SWORD_ASAN
  // A task that is finished is never coming back, so it saves no token; the
  // destination is the stack it arrived from.
  __sanitizer_start_switch_fiber(done ? nullptr : &f->fake_stack,
                                 f->from_bottom, f->from_size);
#else
  (void)done;
#endif
  sword_ctx_switch(&f->sp, f->sched_sp);
  arrived(f);
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
  watch_for_overflow();
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
  watch_for_overflow();
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
    tl_worker = 0; // the thread that starts the pool owns queue 0
    watch_for_overflow();
    atexit(stop_pool);
    running.store(p, std::memory_order_release);
    return p;
  }();
  return *instance;
}

// The queues exist from the start; the threads that serve them are started by
// the first spawn. A program whose only task is `main` runs on the thread the
// process came with, and a thread per core for it would be a cost nobody asked
// for.
void start_workers(Pool &p) {
  if (p.workers_up.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> held(p.start_lock);
  if (p.workers_up.load(std::memory_order_relaxed)) return;
  if (p.stopping.load(std::memory_order_acquire)) return;
  for (size_t i = 1; i < p.workers.size(); i++)
    p.threads.emplace_back(worker_loop, (int)i);
  p.workers_up.store(true, std::memory_order_release);
}

void stop_pool() {
  Pool &p = pool();
  sword_poll_stop();
  p.stopping.store(true, std::memory_order_release);
  p.wake.notify_all();
  // Against a spawn landing at the same moment as the exit: whoever holds this
  // has either started the threads already or will see `stopping` and not.
  std::lock_guard<std::mutex> held(p.start_lock);
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
      fprintf(stderr, "sword: no room for another task stack. The limit is %zu"
                      " live tasks, or the kernel refused the mapping.\n",
              kMaxSlabs * kStacksPerSlab);
      abort();
    }
    p.stacks.fetch_add(1, std::memory_order_relaxed);
  }
  p.started.fetch_add(1, std::memory_order_relaxed);
  f->task = task;
  prepare(f);
  return f;
}

// Stacks go back on the worker's own pile rather than to the kernel: mapping
// one is far more expensive than keeping it.
void retire_fiber(Fiber *f) {
  Pool &p = pool();
  Worker &w = *p.workers[tl_worker >= 0 ? tl_worker : 0];
  p.finished.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> held(w.lock);
  if (w.stacks.size() < 64) {
    w.stacks.push_back(f);
    return;
  }
  p.stacks.fetch_sub(1, std::memory_order_relaxed);
#ifdef SWORD_TSAN
  if (f->tsan) __tsan_destroy_fiber(f->tsan);
#endif
  unmap_stack(f);
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
// A task waiting on several guards at once, which is what `select` is. One of
// these per (task, guard) pair, living on the waiting task's own stack for as
// long as it waits — the task is parked, so the stack is not going anywhere.
struct Selector {
  Fiber *f;
  Selector *next;
};

struct Guard {
  int32_t held;
  uint64_t owner; // which thread, so locking twice is a diagnostic not a hang
  // Tasks waiting for this value to change, newest first. Only ever touched
  // with the guard held, so the list needs no atomics of its own.
  Fiber *waiters;
  // Tasks waiting on this value *and others*. They are woken but not removed:
  // the node belongs to the task, and it takes its own nodes out once it is
  // awake and has decided what to do.
  Selector *selectors;
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
  Fiber *f = tl_fiber;
  arrived(f);
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

// Waits for somebody else to change the value. The guard is released while the
// task is down and taken back before this returns, so the caller sees the same
// invariants it had — except that they may have changed, which is why a caller
// loops rather than testing once.
void sword_mutex_wait(void *blob) {
  Guard *g = (Guard *)blob;
  Fiber *f = tl_fiber;
  if (!f) {
    // Nothing to put down: let go, wait a moment, take it back. Coarse, but
    // only code outside any task ends up here.
    sword_mutex_unlock(blob);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    sword_mutex_lock(blob);
    return;
  }

  f->waiting_next = g->waiters;
  g->waiters = f;
  f->state.store(FIBER_PARKING, std::memory_order_release);
  sword_mutex_unlock(blob);
  leave(f, false);
  // Woken, possibly on another thread, and the guard is somebody else's now.
  sword_mutex_lock(blob);
}

// Waking a fiber that is already awake is not a mistake: only the move out of
// PARKED queues it, so a second wake is a no-op. That is what lets a selector be
// notified by any of the guards it is on.
void wake_one(Fiber *f) {
  if (f->state.exchange(FIBER_READY, std::memory_order_acq_rel) == FIBER_PARKED)
    make_runnable(f);
}

void wake_selectors(Guard *g) {
  for (Selector *s = g->selectors; s; s = s->next) wake_one(s->f);
}

void sword_mutex_notify(void *blob) {
  Guard *g = (Guard *)blob;
  // A selector is waiting for anything to change here, so it is told even when
  // the notify was meant for one particular waiter.
  wake_selectors(g);
  Fiber *f = g->waiters;
  if (!f) return;
  g->waiters = f->waiting_next;
  f->waiting_next = nullptr;
  wake_one(f);
}

void sword_mutex_notify_all(void *blob) {
  Guard *g = (Guard *)blob;
  wake_selectors(g);
  Fiber *f = g->waiters;
  g->waiters = nullptr;
  while (f) {
    Fiber *next = f->waiting_next;
    f->waiting_next = nullptr;
    wake_one(f);
    f = next;
  }
}

// Waiting on several guards at once, in three steps, because the order is the
// whole correctness argument.
//
// `watch` puts the task on every guard's list *before* the caller looks at what it
// is waiting for. That is what makes it safe: a change that lands between the
// caller's look and the park has to find somebody on a list, or the wake is lost
// and the task sleeps for good. It cost one flaky test to learn that doing it the
// other way round is wrong.
//
// The task is marked as parking before it goes on any list, so a notify arriving
// during registration finds it in PARKING, leaves it alone, and the handoff in
// `after_enter` queues it instead of parking it.
int64_t sword_mutex_watch_bytes(void) { return (int64_t)sizeof(Selector); }

void sword_mutex_watch(void **blobs, int64_t n, void *nodes) {
  Fiber *f = tl_fiber;
  if (!f || n <= 0) return;
  Selector *slots = (Selector *)nodes;
  f->state.store(FIBER_PARKING, std::memory_order_release);
  for (int64_t i = 0; i < n; i++) {
    Guard *g = (Guard *)blobs[i];
    slots[i].f = f;
    sword_mutex_lock(g);
    slots[i].next = g->selectors;
    g->selectors = &slots[i];
    sword_mutex_unlock(g);
  }
}

// Returns at once if anything notified since `watch`: the state is no longer
// PARKING, so the handoff queues the task rather than putting it down.
void sword_mutex_park(void) {
  Fiber *f = tl_fiber;
  if (!f) {
    // Outside a task there is nothing to put down, so this is a short sleep and
    // the caller looks again — which is what a `select` outside the scheduler is.
    sword_blocking_enter();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    sword_blocking_exit();
    return;
  }
  leave(f, false);
}

// Everything watching this guard *but the caller*. A task that has just made
// itself a receiver has changed what somebody else's `select` can do, and there
// is no `wait` on the other side to notify — only a selector. Skipping the caller
// is not an optimisation: a select registers its own node before it looks, so
// waking itself here would turn the park that follows into a spin.
void sword_mutex_notify_watchers(void *blob) {
  Guard *g = (Guard *)blob;
  Fiber *me = tl_fiber;
  for (Selector *s = g->selectors; s; s = s->next)
    if (s->f != me) wake_one(s->f);
}

void sword_mutex_unwatch(void **blobs, int64_t n, void *nodes) {
  Fiber *f = tl_fiber;
  if (!f || n <= 0) return;
  Selector *slots = (Selector *)nodes;
  for (int64_t i = 0; i < n; i++) {
    Guard *g = (Guard *)blobs[i];
    sword_mutex_lock(g);
    Selector **link = &g->selectors;
    while (*link && *link != &slots[i]) link = &(*link)->next;
    if (*link) *link = slots[i].next;
    sword_mutex_unlock(g);
  }
  f->state.store(FIBER_RUNNING, std::memory_order_release);
}
}

namespace {

// Instead of idling, a thread waiting on a scope runs whatever work it can
// find. That is what makes nested scopes safe from deadlock.
//
// When there is nothing to run it yields for a while and then starts sleeping. A
// join that waits on something slow — a task on a two-second timer, a server
// shutting down — used to spin a whole core for as long as it took, which is a core
// nobody was using for anything. The cap is a millisecond, so the join answers
// promptly once work appears.
void drain_until(Scope *scope) {
  Pool &p = pool();
  int idle = 0;
  int64_t nap = 50000; // nanoseconds, doubling to a millisecond
  while (scope->outstanding.load(std::memory_order_acquire) > 0) {
    if (Fiber *f = find_ready(tl_worker)) {
      resume_fiber(f);
    } else if (Task *task = find_task(tl_worker)) {
      run_task(task);
    } else if (idle++ < 64) {
      std::this_thread::yield();
      continue;
    } else {
      // Waiting on the pool rather than sleeping on the clock: whatever makes
      // a task runnable notifies it, and this may be the only thread there is —
      // `main` is a task, and a program that never spawns has nobody else to
      // pick it up when the poller wakes it. The nap stays as the ceiling.
      std::unique_lock<std::mutex> held(p.sleep_lock);
      p.wake.wait_for(held, std::chrono::nanoseconds(nap));
      if (nap < 1000000) nap *= 2;
      continue;
    }
    // Something ran, so whatever this was waiting for may be closer now.
    idle = 0;
    nap = 50000;
  }
}

// --- work that cannot be put down ----------------------------------------

// A file read is never "not ready yet" — the wait is the disk, and no poller has
// anything to say about it. The blocking hints answer that by letting the thread
// stop and hiring a replacement, which works but costs a thread per call in
// flight: forty tasks reading files meant forty threads.
//
// These threads are the other answer. They are not scheduler capacity and never
// run Sword code, so a task handing one a call is put down like any other wait,
// and the worker goes on to something else. What it costs is bounded by the pool
// rather than by how many tasks are reading at once.
struct Job {
  int64_t (*fn)(void *);
  void *arg;
  int64_t result;
  Fiber *waiter;
};

struct Offload {
  std::mutex lock;
  std::condition_variable wake;
  std::vector<Job *> queue;
  int64_t threads = 0; // started and not yet retired
  int64_t idle = 0;    // of those, waiting for something to do
  int64_t ceiling = 0; // 0 until the first call decides it
};

Offload &offload_pool() {
  static Offload *instance = new Offload();
  return *instance;
}

// Long enough that a program reading a directory of files keeps its threads,
// short enough that one that read a config file at startup gives them back.
const int kOffloadIdleMillis = 1000;

void offload_loop() {
  Offload &o = offload_pool();
  while (true) {
    Job *job = nullptr;
    {
      std::unique_lock<std::mutex> held(o.lock);
      o.idle++;
      bool got = o.wake.wait_for(held,
                                 std::chrono::milliseconds(kOffloadIdleMillis),
                                 [&o] { return !o.queue.empty(); });
      o.idle--;
      if (!got) {
        o.threads--;
        return; // nothing came; give the thread back
      }
      job = o.queue.back();
      o.queue.pop_back();
    }
    job->result = job->fn(job->arg);
    // The job lives on the waiting task's stack, so nothing may touch it after
    // the handoff: read what is needed first.
    Fiber *f = job->waiter;
    if (f->state.exchange(FIBER_READY, std::memory_order_acq_rel) ==
        FIBER_PARKED)
      make_runnable(f);
  }
}

void submit(Job *job) {
  Offload &o = offload_pool();
  std::lock_guard<std::mutex> held(o.lock);
  if (o.ceiling == 0) {
    unsigned cores = std::thread::hardware_concurrency();
    int64_t fallback = cores > 4 ? (int64_t)cores : 4;
    o.ceiling = env_count("SWORD_IO_THREADS", fallback);
    if (o.ceiling > 256) o.ceiling = 256;
  }
  o.queue.push_back(job);
  // Another thread only while every one there is busy: a queue behind a busy
  // disk is cheaper than a thread nobody needed.
  if (o.idle == 0 && o.threads < o.ceiling) {
    o.threads++;
    std::thread(offload_loop).detach();
  }
  o.wake.notify_one();
}

int64_t offload_threads() {
  Offload &o = offload_pool();
  std::lock_guard<std::mutex> held(o.lock);
  return o.threads;
}

// Puts a task on this thread's queue. What a spawn does on top of it is start
// the workers and cover for anyone parked; the main task wants neither, since
// it is the only thing there is to run until it says otherwise.
void queue_task(Scope *scope, sword_task_fn fn, const void *args,
                int64_t size) {
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
  queue_task((Scope *)blob, fn, args, size);
  Pool &p = pool();
  // A spawn is the first sign that the program has more than one thing to do,
  // which is when the rest of the workers are worth starting.
  start_workers(p);
  // Work arriving while threads are parked is the other half of the hiring
  // rule: without this, a task spawned after everyone blocked would wait for
  // the next blocking call to notice it.
  cover_for_parked(p);
}

// `main` is a task like anything else. Parking is a fiber's trick, so before
// this the wait in a program whose whole job is one loop over one descriptor
// could not work: sword_park_fd found no fiber, answered -1, and a loop
// written to stop on an error stopped on its first read.
uint16_t sword_run_main(sword_task_fn fn, const void *args, int64_t size) {
  alignas(8) unsigned char blob[SWORD_SCOPE_SIZE];
  sword_scope_begin(blob);
  queue_task((Scope *)blob, fn, args, size);
  return sword_scope_end(blob);
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

// Waits for a time without holding the thread. -1 when there is no task to put
// down, so the caller sleeps the old way.
int sword_park_timer(int64_t deadline_ns) {
  Fiber *f = tl_fiber;
  if (!f) return -1;
  sword_poll_start(on_ready);
  f->wake_result = SWORD_POLL_TIMEOUT;
  f->state.store(FIBER_PARKING, std::memory_order_release);
  sword_poll_sleep(f, deadline_ns);
  leave(f, false);
  return 0;
}

int32_t sword_in_task(void) { return tl_fiber != nullptr; }

void sword_runtime_stats(struct sword_stats *out) {
  memset(out, 0, sizeof(*out));
  // Settings first: they are answerable before a single task has run.
  out->stack_bytes = (int64_t)stack_bytes();
  Pool *p = running.load(std::memory_order_acquire);
  if (!p) return;
  // Until the first spawn there is one worker: the thread the program came
  // with, running `main`. Reporting the target before that would be reporting
  // threads that do not exist.
  int64_t workers = p->workers_up.load(std::memory_order_acquire) ? p->target : 1;
  out->threads = workers + p->hired.load(std::memory_order_relaxed);
  out->queued = p->pending.load(std::memory_order_relaxed);
  out->parked = p->parked.load(std::memory_order_relaxed);
  out->stacks = p->stacks.load(std::memory_order_relaxed);
  out->started = p->started.load(std::memory_order_relaxed);
  out->finished = p->finished.load(std::memory_order_relaxed);
  out->io_threads = offload_threads();
}

void sword_forget_fd(int32_t fd) { sword_poll_forget((int)fd); }

// Hands one call to a thread set aside for calls that cannot be put down, and
// puts the calling task down until it comes back. Outside a task there is
// nothing to put down, so it runs here and the blocking hints cover for it — the
// same as before this existed.
int64_t sword_offload(int64_t (*fn)(void *), void *arg) {
  Fiber *f = tl_fiber;
  if (!f) {
    sword_blocking_enter();
    int64_t result = fn(arg);
    sword_blocking_exit();
    return result;
  }
  // On this task's own stack, which stays where it is while the task is down.
  Job job{fn, arg, 0, f};
  f->state.store(FIBER_PARKING, std::memory_order_release);
  submit(&job);
  leave(f, false);
  return job.result;
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
