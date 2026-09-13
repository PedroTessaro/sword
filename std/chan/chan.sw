package chan

import "std/mem"

error Closed = "the channel is closed"

// What is behind `chan[T]`, `<-`, `close` and `select`. Nobody imports this: the
// compiler brings it in when a program mentions a channel, and rewrites every
// operation on one into a call here.
//
// The queue itself lives in the runtime, because `select` has to hold several
// channels at once and register a waiter on each, and neither of those is
// expressible with a lexical `lock` over a count only known at run time. What
// stays here is the part that has to know about types — and the allocation, so
// that a channel still takes the memory it uses from whoever asked for it.
extern func sword_chan_bytes(capacity i64, elem i64) i64
extern func sword_chan_init(mem [*]u8, capacity i64, elem i64)
extern func sword_chan_send(chan [*]u8, value [*]u8) i32
extern func sword_chan_recv(chan [*]u8, into [*]u8) i32
extern func sword_chan_try_send(chan [*]u8, value [*]u8) i32
extern func sword_chan_try_recv(chan [*]u8, into [*]u8) i32
extern func sword_chan_close(chan [*]u8)
extern func sword_chan_len(chan [*]u8) i64
extern func sword_chan_cap(chan [*]u8) i64
extern func sword_chan_closed(chan [*]u8) i32

// A handle: copying one copies the handle, and both copies are the same channel.
// That is what lets a channel be handed to a task by value.
struct Chan[T] {
    state []u8
}

// Capacity is how many values may be in flight before a send waits. Zero makes
// it a handover: the sender waits for a receiver to take the value, which is how
// two tasks meet at a point rather than through a queue.
func New[T](mut a mem.Allocator, capacity u64) !Chan[T] {
    size := u64(sword_chan_bytes(i64(capacity), i64(sizeof[T]())))
    // Sixteen, not alignof[T](): the runtime's own header sits in front of the
    // values and holds a mutex, and an unaligned mutex is not a mutex.
    room := a.Alloc(size, 16) orelse return error.OutOfMemory
    sword_chan_init(room, i64(capacity), i64(sizeof[T]()))
    return Chan[T]{state: room[0..size]}
}

// Gives back the memory the channel was built with. Everything must be done with
// it: nothing here checks, because there would be nobody left to tell.
func (c Chan[T]) Free(mut a mem.Allocator) {
    mem.Free(a, c.state)
}

// Waits while there is no room. Fails once the channel is closed, because a
// value nobody will ever take is a mistake rather than a wait.
func (c Chan[T]) Send(v T) !void {
    mut value := v
    if sword_chan_send(c.state.ptr, [*]u8(&value)) != 0 {
        return error.Closed
    }
}

// Waits while the channel is empty. Nil once it is closed and drained, which is
// how a receiver learns there will be no more:
//
//     for job := <-jobs {
//         try handle(job)
//     }
func (c Chan[T]) Recv() ?T {
    mut into := [1]T{}
    if sword_chan_recv(c.state.ptr, [*]u8(&into[0])) != 1 {
        return nil
    }
    return into[0]
}

// Takes one if there is one, without waiting.
func (c Chan[T]) TryRecv() ?T {
    mut into := [1]T{}
    if sword_chan_try_recv(c.state.ptr, [*]u8(&into[0])) != 1 {
        return nil
    }
    return into[0]
}

// Puts one in if it can go without waiting. On a handover that means a receiver
// has to be parked already — otherwise the send would be a wait, which is the
// one thing this form promises not to do.
func (c Chan[T]) TrySend(v T) bool {
    mut value := v
    return sword_chan_try_send(c.state.ptr, [*]u8(&value)) == 1
}

// No more values will be sent. Whatever is already in the channel is still
// received; after that every receive answers nil. Closing twice is harmless,
// which matters because whoever closes is often not whoever knows.
func (c Chan[T]) Close() {
    sword_chan_close(c.state.ptr)
}

func (c Chan[T]) Len() u64 {
    return u64(sword_chan_len(c.state.ptr))
}

// Zero for a handover, which is the capacity it was asked for.
func (c Chan[T]) Cap() u64 {
    return u64(sword_chan_cap(c.state.ptr))
}

func (c Chan[T]) Closed() bool {
    return sword_chan_closed(c.state.ptr) == 1
}

// Where this channel's state is, for `select` — the only thing that needs several
// channels at once. Not for hand-written code: the operators do everything else.
func (c Chan[T]) Address() [*]u8 {
    return c.state.ptr
}
