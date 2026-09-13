// expect: 42
// expect-output: select works
// Waiting on several channels at once, which is the thing a server does all day:
// take work, and notice when somebody says stop. The parts worth checking are the
// ones that are easy to get wrong — that a case which is never ready does not
// block the others, that a closed channel answers rather than hanging, that a
// send case waits for room, that `default` never waits, and that a case which is
// always ready does not starve the rest.

import "std/io"
import "std/mem"
import "std/time"

// Takes work until somebody says stop. The shape this whole feature exists for.
func work(jobs chan[u64], quit chan[u64], mut sum *atomic[u64],
          mut stopped *atomic[u64]) !void {
    for {
        select {
        case job := <-jobs:
            j := job orelse return
            sum.Add(j)
        case <-quit:
            stopped.Add(1)
            return
        }
    }
}

// Reads one channel and fills the other, with no buffer on either: every case
// here has to wait for somebody on the far side.
func pair(left chan[u64], right chan[u64], mut cases *atomic[u64],
          mut received *atomic[u64]) !void {
    for i in 0..6 {
        select {
        case v := <-left:
            if v != nil {
                received.Add(1)
            }
            cases.Add(1)
        case right <- 100:
            cases.Add(1)
        }
    }
}

// Three values and no close: once they are taken, that case is simply never
// ready again, which is what leaves the send case as the only way forward.
func feed(c chan[u64], count u64) !void {
    for i in 0..count {
        c <- i catch break
    }
}

func drain(c chan[u64], count u64, mut total *atomic[u64]) !void {
    for i in 0..count {
        v := <-c orelse break
        total.Add(v)
    }
}

// Two sources, one of them always ready. Without a rotating start the first case
// would be the only one that ever ran.
func both(fast chan[u64], slow chan[u64], mut from_fast *atomic[u64],
          mut from_slow *atomic[u64]) !void {
    for i in 0..40 {
        select {
        case v := <-fast:
            from_fast.Add(v orelse 0)
        case v := <-slow:
            from_slow.Add(v orelse 0)
        }
    }
}

func main() !int {
    mut backing := [262144]u8{}
    mut arena := mem.NewArena(backing[..])

    // Work until told to stop. The values are sent before the stop, so all ten
    // arrive: a select that could take either has to take what is there.
    mut jobs := try chan[u64](&arena, 16)
    mut quit := try chan[u64](&arena, 1)
    mut sum := atomic[u64](0)
    mut stopped := atomic[u64](0)
    scope {
        spawn work(jobs, quit, &sum, &stopped)
        for i in 0..10 {
            jobs <- u64(i) catch {}
        }
        // Long enough for the worker to have drained them; the stop then has
        // nothing to race with.
        time.Sleep(time.Millis(30))
        quit <- 1 catch {}
    }
    if sum.Load() != 45 || stopped.Load() != 1 {
        return 1
    }

    // A closed channel is an answer, not a wait: the case fires with nothing in
    // it, which is what lets a loop end instead of hanging.
    mut shut := try chan[u64](&arena, 2)
    close(shut)
    mut answered := false
    select {
    case v := <-shut:
        answered = v == nil
    }
    if !answered {
        return 2
    }

    // `default` never waits, whether the channel is empty or full.
    mut quiet := try chan[u64](&arena, 1)
    mut which u64 = 0
    select {
    case v := <-quiet:
        which = 1
    default:
        which = 2
    }
    if which != 2 {
        return 3
    }
    quiet <- 9 catch {}
    select {
    case v := <-quiet:
        which = v orelse 0
    default:
        which = 99
    }
    if which != 9 {
        return 4
    }

    // Two handovers, no buffer: one side of the select has to find a receiver
    // and the other a sender, and neither can be hurried.
    mut left := try chan[u64](&arena, 0)
    mut right := try chan[u64](&arena, 0)
    mut cases := atomic[u64](0)
    mut received := atomic[u64](0)
    mut taken := atomic[u64](0)
    scope {
        spawn pair(left, right, &cases, &received)
        spawn feed(left, 3)
        spawn drain(right, 3, &taken)
    }
    // Six cases in all: three values out of `left`, and three sends into `right`
    // once `left` has nothing more to give.
    if cases.Load() != 6 || received.Load() != 3 {
        try io.Printf("cases {} received {}\n", cases.Load(), received.Load())
        return 5
    }
    if taken.Load() != 300 {
        return 6
    }

    // Fairness: one channel always has something, the other rarely does. Both
    // have to get a turn, which is what the rotating start is for.
    // Both filled in advance, so nothing in here can wait on anything: what is
    // being measured is which case the select picks, not who got there first.
    mut fast := try chan[u64](&arena, 64)
    mut slow := try chan[u64](&arena, 8)
    for i in 0..60 {
        try fast <- 1
    }
    for i in 0..8 {
        try slow <- 1
    }
    mut from_fast := atomic[u64](0)
    mut from_slow := atomic[u64](0)
    scope {
        spawn both(fast, slow, &from_fast, &from_slow)
    }
    if from_fast.Load() == 0 || from_slow.Load() == 0 {
        try io.Printf("fast {} slow {}\n", from_fast.Load(), from_slow.Load())
        return 7
    }

    try io.Printf("select works: {} of {} cases came from the quiet side\n",
                  from_slow.Load(), from_slow.Load() + from_fast.Load())
    return 42
}
