// expect: 42
// expect-output: channels work
// Four consumers and one producer over one channel, then a channel of one slot
// against a slow consumer. Waiting on a channel costs a stack, not a thread,
// which is why the peak thread count stays at the worker count.

import "std/chan"
import "std/io"
import "std/mem"
import "std/runtime"
import "std/time"

func consume(c *chan.Chan[u64], mut total *atomic[u64],
             mut seen *atomic[u64]) !void {
    for v := c.Recv() {
        total.Add(v)
        seen.Add(1)
    }
}

func produce(c *chan.Chan[u64], upto u64) !void {
    for i in 0..upto {
        try c.Send(i)
    }
    c.Close()
}

func slowly(c *chan.Chan[u64], mut seen *atomic[u64]) !void {
    for v := c.Recv() {
        time.Sleep(time.Millis(5))
        seen.Add(1)
    }
}

func quickly(c *chan.Chan[u64], mut peak *atomic[u64]) !void {
    for i in 0..30 {
        try c.Send(u64(i))
        r := runtime.Read()
        if u64(r.Threads) > peak.Load() {
            peak.Store(u64(r.Threads))
        }
    }
    c.Close()
}

func main() !int {
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])

    // Fan-out: every value reaches exactly one consumer, and closing lets all
    // four loops end.
    mut jobs := try chan.New[u64](&arena, 8)
    mut total := atomic[u64](0)
    mut seen := atomic[u64](0)
    scope {
        for w in 0..4 {
            spawn consume(&jobs, &total, &seen)
        }
        spawn produce(&jobs, 100)
    }
    if seen.Load() != 100 || total.Load() != 4950 {
        return 1
    }

    // What is already in a closed channel is still received.
    mut left := try chan.New[u64](&arena, 4)
    try left.Send(1)
    try left.Send(2)
    left.Close()
    if (left.Recv() orelse 0) != 1 || (left.Recv() orelse 0) != 2 {
        return 2
    }
    if left.Recv() != nil {
        return 3
    }
    // And sending into one is a mistake rather than a wait.
    mut refused := false
    left.Send(3) catch {
        refused = true
    }
    if !refused {
        return 4
    }

    // Neither of the impatient forms waits.
    mut room := try chan.New[u64](&arena, 2)
    if room.TryRecv() != nil {
        return 5
    }
    if !room.TrySend(1) || !room.TrySend(2) || room.TrySend(3) {
        return 6
    }
    if room.Len() != 2 || room.Cap() != 2 {
        return 7
    }

    // Backpressure: one slot against a consumer that takes five milliseconds
    // each. Thirty of those cannot go faster than the consumer.
    mut tight := try chan.New[u64](&arena, 1)
    mut slow_seen := atomic[u64](0)
    mut peak := atomic[u64](0)
    start := time.Now()
    scope {
        spawn slowly(&tight, &slow_seen)
        spawn quickly(&tight, &peak)
    }
    took := time.Since(start)
    if slow_seen.Load() != 30 {
        return 8
    }
    if took.AsMillis() < 100 {
        return 9
    }

    try io.Printf("channels work: {} through, held back to {}ms on {} threads\n",
                  seen.Load(), took.AsMillis(), peak.Load())
    return 42
}
