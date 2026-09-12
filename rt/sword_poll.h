#pragma once

#include <stdint.h>

// Readiness notification, one kqueue or epoll behind a seam that knows nothing
// about tasks. The scheduler hands it an opaque token and a function to call
// when the descriptor is ready or the deadline has passed; what that token
// means is the scheduler's business.
extern "C" {

enum {
  SWORD_POLL_READY = 0,
  SWORD_POLL_FAILED = -1,
  SWORD_POLL_TIMEOUT = -2,
};

typedef void (*sword_wake_fn)(void *token, int result);

// Starts the poller thread on the first call. Safe to call from anywhere.
void sword_poll_start(sword_wake_fn wake);

// Waits for `fd` to be readable (or writable), then calls the wake function
// exactly once. `deadline_ns` is an absolute time on the monotonic clock, or
// zero to wait as long as it takes.
void sword_poll_wait(int fd, int writable, void *token, int64_t deadline_ns);

// Drops anything still waiting on `fd`, for a descriptor about to be closed.
void sword_poll_forget(int fd);

// Waits for a time rather than for a descriptor. The wake function is called
// once, with SWORD_POLL_TIMEOUT.
void sword_poll_sleep(void *token, int64_t deadline_ns);

void sword_poll_stop(void);
}
