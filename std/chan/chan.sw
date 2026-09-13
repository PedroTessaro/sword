package chan

import "std/mem"

error Closed = "the channel is closed"

// The implementation behind `chan[T]`, `<-` and `close`. Nobody imports this: the
// compiler brings it in when a program mentions a channel, and every operation on
// one is rewritten into a call here.
//
// It is written in Sword rather than in the compiler or the runtime because it
// needs nothing they have to offer. A `shared` value holds the state, Wait and
// Notify do the waiting, and waiting on either costs a stack rather than a
// thread: the task is put down and the worker goes to somebody else.
struct ring[T] {
    items  []T
    head   u64 // where the next value is taken from
    count  u64 // how many are in it
    // A channel of capacity zero is a handover rather than a queue: the sender
    // waits until a receiver has actually taken the value. The slot is still
    // one element, because the value has to be somewhere while it changes hands.
    direct bool
    // Receivers parked on an empty channel. A handover with nobody waiting is
    // not ready, which is what the impatient forms have to be able to tell.
    waiting u64
    closed  bool
}

// One element, so that a channel is a handle: copying one copies the handle and
// both copies are the same channel, which is what lets it be handed to a task.
struct Chan[T] {
    state []shared[ring[T]]
}

// Capacity is how many values may be in flight before a send waits. Zero makes
// it a handover: the sender waits for a receiver to take the value, which is how
// two tasks meet at a point rather than through a queue.
func New[T](mut a mem.Allocator, capacity u64) !Chan[T] {
    mut slots := capacity
    direct := capacity == 0
    if direct {
        slots = 1
    }
    room := mem.Alloc[T](a, slots) orelse return error.OutOfMemory
    mut state := mem.Alloc[shared[ring[T]]](a, 1) orelse return error.OutOfMemory
    state[0] = shared[ring[T]](ring[T]{items: room, head: 0, count: 0,
                                       direct: direct, waiting: 0,
                                       closed: false})
    return Chan[T]{state: state}
}

// Gives back what the channel was built with. Everything must be done with it.
func (c Chan[T]) Free(mut a mem.Allocator) {
    lock r := &c.state[0] {
        mem.Free(a, r.items)
        r.items = r.items[0..0]
    }
    mem.Free(a, c.state)
}

// Waits while there is no room. Fails once the channel is closed, because a
// value nobody will ever take is a mistake rather than a wait.
func (c Chan[T]) Send(v T) !void {
    lock r := &c.state[0] {
        for r.count >= r.items.len && !r.closed {
            c.state[0].Wait()
        }
        if r.closed {
            return error.Closed
        }
        at := (r.head + r.count) % r.items.len
        r.items[at] = v
        r.count += 1
        c.state[0].NotifyAll()

        // A handover is not done until somebody has taken it.
        for r.direct && r.count > 0 && !r.closed {
            c.state[0].Wait()
        }
    }
}

// Waits while the channel is empty. Nil once it is closed and drained, which is
// how a receiver learns there will be no more:
//
//     for job := <-jobs {
//         try handle(job)
//     }
func (c Chan[T]) Recv() ?T {
    lock r := &c.state[0] {
        r.waiting += 1
        for r.count == 0 && !r.closed {
            c.state[0].Wait()
        }
        r.waiting -= 1
        if r.count == 0 {
            return nil
        }
        v := r.items[r.head]
        r.head = (r.head + 1) % r.items.len
        r.count -= 1
        c.state[0].NotifyAll()
        return v
    }
    return nil
}

// Takes one if there is one, without waiting.
func (c Chan[T]) TryRecv() ?T {
    lock r := &c.state[0] {
        if r.count == 0 {
            return nil
        }
        v := r.items[r.head]
        r.head = (r.head + 1) % r.items.len
        r.count -= 1
        c.state[0].NotifyAll()
        return v
    }
    return nil
}

// Puts one in if it can go without waiting. On a handover that means a receiver
// has to be parked already — otherwise the send would be a wait, which is the
// one thing this form promises not to do.
func (c Chan[T]) TrySend(v T) bool {
    lock r := &c.state[0] {
        if r.closed || r.count >= r.items.len {
            return false
        }
        if r.direct && r.waiting == 0 {
            return false
        }
        at := (r.head + r.count) % r.items.len
        r.items[at] = v
        r.count += 1
        c.state[0].NotifyAll()
        return true
    }
    return false
}

// No more values will be sent. Whatever is already in the channel is still
// received; after that every receive answers nil. Closing twice is harmless,
// which matters because whoever closes is often not whoever knows.
func (c Chan[T]) Close() {
    lock r := &c.state[0] {
        r.closed = true
        c.state[0].NotifyAll()
    }
}

func (c Chan[T]) Len() u64 {
    lock r := &c.state[0] {
        return r.count
    }
    return 0
}

// Zero for a handover, which is the capacity it was asked for.
func (c Chan[T]) Cap() u64 {
    lock r := &c.state[0] {
        if r.direct {
            return 0
        }
        return r.items.len
    }
    return 0
}

func (c Chan[T]) Closed() bool {
    lock r := &c.state[0] {
        return r.closed
    }
    return false
}
