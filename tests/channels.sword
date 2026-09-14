// expect: 42
// expect-output: channels work
// Four consumers and one producer over one channel, then a handover with no
// buffer at all, then a channel of one slot against a slow consumer. Waiting on
// a channel costs a stack and not a thread, which is why the thread count stays
// at the worker count no matter how many tasks are parked on one.

import "std/io"
import "std/mem"
import "std/runtime"
import "std/time"

func consume(c chan[u64], mut total *atomic[u64], mut seen *atomic[u64]) !void {
    for v := <-c {
        total.Add(v)
        seen.Add(1)
    }
}

func produce(c chan[u64], upto u64) !void {
    for i in 0..upto {
        try c <- i
    }
    close(c)
}

func slowly(c chan[u64], mut seen *atomic[u64]) !void {
    for v := <-c {
        time.Sleep(time.Millis(5))
        seen.Add(1)
    }
}

func quickly(c chan[u64], mut peak *atomic[u64]) !void {
    for i in 0..30 {
        try c <- u64(i)
        r := runtime.Read()
        if u64(r.Threads) > peak.Load() {
            peak.Store(u64(r.Threads))
        }
    }
    close(c)
}

func main() !int {
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])

    // Fan-out: every value reaches exactly one consumer, and closing lets all
    // four loops end. The channel is handed to the tasks by value — it is a
    // handle, and both copies are the same channel.
    mut jobs := try chan[u64](&arena, 8)
    mut total := atomic[u64](0)
    mut seen := atomic[u64](0)
    scope {
        for w in 0..4 {
            spawn consume(jobs, &total, &seen)
        }
        spawn produce(jobs, 100)
    }
    if seen.Load() != 100 || total.Load() != 4950 {
        return 1
    }
    if jobs.Cap() != 8 {
        return 2
    }

    // No buffer: the sender waits until a receiver has taken the value, so the
    // two meet at a point rather than through a queue.
    mut hand := try chan[u64](&arena, 0)
    mut met := atomic[u64](0)
    mut count := atomic[u64](0)
    scope {
        spawn consume(hand, &met, &count)
        spawn produce(hand, 6)
    }
    if count.Load() != 6 || met.Load() != 15 || hand.Cap() != 0 {
        return 3
    }

    // What is already in a closed channel is still received.
    mut left := try chan[u64](&arena, 4)
    try left <- 1
    try left <- 2
    close(left)
    if (<-left orelse 0) != 1 || (<-left orelse 0) != 2 {
        return 4
    }
    if <-left != nil {
        return 5
    }
    // And sending into one is a mistake rather than a wait.
    mut refused := false
    left <- 3 catch {
        refused = true
    }
    if !refused {
        return 6
    }

    // Neither of the impatient forms waits.
    mut room := try chan[u64](&arena, 2)
    if room.TryRecv() != nil {
        return 7
    }
    if !room.TrySend(1) || !room.TrySend(2) || room.TrySend(3) {
        return 8
    }
    if room.Len() != 2 || room.Cap() != 2 {
        return 9
    }
    // On a handover there is nobody to hand to, so the impatient send says no
    // rather than turning into a wait.
    mut alone := try chan[u64](&arena, 0)
    if alone.TrySend(1) {
        return 10
    }

    // Backpressure: one slot against a consumer that takes five milliseconds
    // each. Thirty of those cannot go faster than the consumer.
    mut tight := try chan[u64](&arena, 1)
    mut slow_seen := atomic[u64](0)
    mut peak := atomic[u64](0)
    start := time.Now()
    scope {
        spawn slowly(tight, &slow_seen)
        spawn quickly(tight, &peak)
    }
    took := time.Since(start)
    if slow_seen.Load() != 30 {
        return 11
    }
    if took.AsMillis() < 100 {
        return 12
    }

    try io.Printf("channels work: {} through, held back to {}ms on {} threads\n",
                  seen.Load(), took.AsMillis(), peak.Load())
    return 42
}
