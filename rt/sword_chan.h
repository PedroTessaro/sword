#pragma once

#include <stdint.h>

// The inside of a channel. It lives here rather than in Sword for one reason:
// `select` has to hold several channels at once and register a waiter on each,
// and neither of those is expressible with a lexical `lock` over a count that is
// only known at run time.
//
// Nothing here allocates. The caller hands over a block of its own memory —
// `sword_chan_bytes` says how much — which is what keeps a channel obeying the
// rule that nothing in this language allocates without being asked.
extern "C" {

// How much memory a channel of this shape needs, header and slots together.
int64_t sword_chan_bytes(int64_t capacity, int64_t elem);
// `mem` must be that many bytes, aligned to 16, and zeroed or not — this writes
// all of it.
void sword_chan_init(void *mem, int64_t capacity, int64_t elem);

// 0 sent, -1 the channel is closed. Waits while there is no room; on a channel of
// capacity zero, waits until a receiver has taken the value.
int32_t sword_chan_send(void *chan, const void *value);
// 1 a value was written to `into`, 0 the channel is closed and empty. Waits while
// it is empty.
int32_t sword_chan_recv(void *chan, void *into);

// The same two without waiting: 1 done, 0 not now.
int32_t sword_chan_try_send(void *chan, const void *value);
int32_t sword_chan_try_recv(void *chan, void *into);

void sword_chan_close(void *chan);
int64_t sword_chan_len(void *chan);
int64_t sword_chan_cap(void *chan);
int32_t sword_chan_closed(void *chan);

// What a case in a `select` is asking for.
enum { SWORD_CHAN_RECV = 0, SWORD_CHAN_SEND = 1 };

// Waits until one of these cases can go, does it, and says which. `ops[i]` is
// RECV or SEND; `values[i]` is where a received value goes or where a sent one
// comes from. Returns the index that fired, or `n` when nothing could go and
// `has_default` was set. `*got` is 1 when a receive produced a value and 0 when
// it found the channel closed and empty — which is a case firing, not a case
// waiting, because a closed channel has an answer.
//
// A send case whose channel is closed can never go, and is skipped.
int64_t sword_chan_select(void **chans, const int32_t *ops, void **values,
                          int64_t n, int32_t has_default, int32_t *got);
}
