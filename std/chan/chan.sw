package chan

import "std/mem"

error Closed = "the channel is closed"

// A queue tasks hand values through. A send waits while it is full and a
// receive waits while it is empty, and waiting here costs a stack rather than a
// thread — the task is put down and the worker goes elsewhere.
//
// Written in Sword rather than built into the compiler, on top of the two
// things the language already has: a `shared` value for the state, and Wait and
// Notify on it for the waiting. There is nothing a channel needs that those do
// not give.
struct ring[T] {
    items  []T
    head   u64 // where the next value is taken from
    count  u64 // how many are in it
    closed bool
    // Senders parked on a full ring and receivers on an empty one both wait on
    // the same value, so a wake has to reach everybody and let them look again.
    // With one queue that is what NotifyAll is for.
}

struct Chan[T] {
    state shared[ring[T]]
}

// Capacity is how many values may be in flight before a send waits. One is
// enough to hand work over; more smooths a producer that comes in bursts.
func New[T](mut a mem.Allocator, capacity u64) !Chan[T] {
    mut want := capacity
    if want == 0 {
        want = 1
    }
    room := mem.Alloc[T](a, want) orelse return error.OutOfMemory
    return Chan[T]{state: shared[ring[T]](
        ring[T]{items: room, head: 0, count: 0, closed: false})}
}

// Frees what the channel was built with. Everything must be done with it.
func (mut c *Chan[T]) Free(mut a mem.Allocator) {
    lock r := &c.state {
        mem.Free(a, r.items)
        r.items = r.items[0..0]
    }
}

// Waits while the channel is full. Fails once it is closed, because a value
// nobody will ever take is a mistake rather than a wait.
func (c *Chan[T]) Send(v T) !void {
    lock r := &c.state {
        for r.count >= r.items.len && !r.closed {
            c.state.Wait()
        }
        if r.closed {
            return error.Closed
        }
        at := (r.head + r.count) % r.items.len
        r.items[at] = v
        r.count += 1
        c.state.NotifyAll()
    }
}

// Waits while the channel is empty. Nil once it is closed and drained, which is
// how a receiver learns there will be no more:
//
//     for job := jobs.Recv() {
//         handle(job)
//     }
func (c *Chan[T]) Recv() ?T {
    lock r := &c.state {
        for r.count == 0 && !r.closed {
            c.state.Wait()
        }
        if r.count == 0 {
            return nil
        }
        v := r.items[r.head]
        r.head = (r.head + 1) % r.items.len
        r.count -= 1
        c.state.NotifyAll()
        return v
    }
    return nil
}

// Takes one if there is one, without waiting.
func (c *Chan[T]) TryRecv() ?T {
    lock r := &c.state {
        if r.count == 0 {
            return nil
        }
        v := r.items[r.head]
        r.head = (r.head + 1) % r.items.len
        r.count -= 1
        c.state.NotifyAll()
        return v
    }
    return nil
}

// Puts one in if there is room, without waiting. False when there is not, or
// when the channel is closed.
func (c *Chan[T]) TrySend(v T) bool {
    lock r := &c.state {
        if r.closed || r.count >= r.items.len {
            return false
        }
        at := (r.head + r.count) % r.items.len
        r.items[at] = v
        r.count += 1
        c.state.NotifyAll()
        return true
    }
    return false
}

// No more values will be sent. Whatever is already in the channel is still
// received; after that every receive answers nil. Closing twice is harmless,
// which matters because the one who closes is often not the one who knows.
func (c *Chan[T]) Close() {
    lock r := &c.state {
        r.closed = true
        c.state.NotifyAll()
    }
}

func (c *Chan[T]) Len() u64 {
    lock r := &c.state {
        return r.count
    }
    return 0
}

func (c *Chan[T]) Cap() u64 {
    lock r := &c.state {
        return r.items.len
    }
    return 0
}

func (c *Chan[T]) Closed() bool {
    lock r := &c.state {
        return r.closed
    }
    return false
}
