#pragma once

#include <stdint.h>

// The ABI between generated code and the scheduler. Every task is a thunk the
// compiler writes: it unpacks a copied argument block and returns an error
// code, where zero means success.
extern "C" {

typedef uint16_t (*sword_task_fn)(void *args);
typedef uint16_t (*sword_chunk_fn)(void *env, int64_t lo, int64_t hi);

// Opaque to generated code, which only ever reserves this much word-aligned
// space on its own frame and passes the address along. The alignment matters:
// there are atomics in here.
enum { SWORD_SCOPE_SIZE = 64 };

void sword_scope_begin(void *scope);
void sword_scope_spawn(void *scope, sword_task_fn fn, const void *args,
                       int64_t size);
uint16_t sword_scope_end(void *scope);

uint16_t sword_parallel_for(int64_t lo, int64_t hi, sword_chunk_fn fn,
                            void *env);

// Brackets a call that parks the thread in the kernel. A parked thread is not
// scheduler capacity, so the pool hires a replacement for as long as it is
// gone: blocking I/O then costs a thread instead of a core. Cheap and safe to
// call when no pool is running, which is the common case for a program that
// never spawns anything.
void sword_blocking_enter(void);
void sword_blocking_exit(void);

// The guard inside a `shared[T]`, which generated code only ever passes the
// address of. All-zero bytes are an unlocked guard, so a shared needs no
// constructor and can sit in an array or a struct field.
enum { SWORD_GUARD_SIZE = 16 };

void sword_mutex_lock(void *guard);
void sword_mutex_unlock(void *guard);

// Waits for a descriptor without holding the thread. Only a task can be put
// down, so this answers -1 when there is none and the caller waits the old way.
// 0 means ready, -2 means the deadline passed.
int sword_park_fd(int32_t fd, int32_t writable, int64_t deadline_ns);
int32_t sword_in_task(void);
// The same for the clock: waits without holding the thread, and answers -1 when
// there is no task to put down.
int sword_park_timer(int64_t deadline_ns);
// Drops anything waiting on a descriptor about to be closed.
void sword_forget_fd(int32_t fd);

// What the scheduler is doing right now. Laid out to match `runtime.Stats` on
// the Sword side field for field, which is why that one is an extern struct.
struct sword_stats {
  int64_t threads;  // workers, including any hired to cover a parked one
  int64_t queued;   // spawned and not yet picked up
  int64_t parked;   // threads stopped in the kernel
  int64_t stacks;   // task stacks alive, in use or pooled
  int64_t started;  // tasks begun since the program did
  int64_t finished; // and tasks ended
};

void sword_runtime_stats(struct sword_stats *out);
}
