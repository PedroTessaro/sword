#pragma once

#include <stdint.h>

// The ABI between generated code and the scheduler. Every task is a thunk the
// compiler writes: it unpacks a copied argument block and returns an error
// code, where zero means success.
extern "C" {

typedef uint16_t (*sword_task_fn)(void *args);
typedef uint16_t (*sword_chunk_fn)(void *env, int64_t lo, int64_t hi);

// Opaque to generated code, which only ever allocates this many bytes on its
// own frame and passes the address along.
enum { SWORD_SCOPE_SIZE = 64 };

void sword_scope_begin(void *scope);
void sword_scope_spawn(void *scope, sword_task_fn fn, const void *args,
                       int64_t size);
uint16_t sword_scope_end(void *scope);

uint16_t sword_parallel_for(int64_t lo, int64_t hi, sword_chunk_fn fn,
                            void *env);
}
