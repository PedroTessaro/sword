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
// zero to wait as long as it takes. `generation` is what sword_poll_generation
// said before the call that answered "not yet"; if the descriptor has been
// forgotten since, the wait fails at once.
void sword_poll_wait(int fd, int writable, void *token, int64_t deadline_ns,
                     uint64_t generation);

// Moves on with every forget. Read before a call that may have to wait.
uint64_t sword_poll_generation(int fd);

// Drops anything still waiting on `fd`, for a descriptor about to be closed,
// and moves its generation on.
void sword_poll_forget(int fd);

// Closes a forgotten descriptor. With kqueue the poller's own thread does it,
// between two waits; elsewhere it happens here. Either way the number is not
// handed out again before the close, since the descriptor stays open until then.
void sword_poll_close(int fd);

// Waits for a time rather than for a descriptor. The wake function is called
// once, with SWORD_POLL_TIMEOUT.
void sword_poll_sleep(void *token, int64_t deadline_ns);

void sword_poll_stop(void);
}
