// expect: 42
// expect-output: caught Terminate, then Interrupt
// A signal handler may do almost nothing safely, so a signal arrives down a
// pipe and is waited for like any other descriptor — without holding a thread.

import "std/io"
import "std/os"
import "std/time"

func listener(mut got *atomic[u64], mut second *atomic[u64]) !void {
    first := try os.WaitSignal()
    got.Store(u64(i32(first)))
    next := try os.WaitSignal()
    second.Store(u64(i32(next)))
}

func sender() !void {
    time.Sleep(time.Millis(30))
    try os.Kill(os.Pid(), os.Signal.Terminate)
    time.Sleep(time.Millis(30))
    try os.Kill(os.Pid(), os.Signal.Interrupt)
}

func main() !int {
    // Without this the first one would end the process rather than be caught.
    try os.Catch(os.Signal.Terminate)
    try os.Catch(os.Signal.Interrupt)

    mut got := atomic[u64](0)
    mut second := atomic[u64](0)
    scope {
        spawn listener(&got, &second)
        spawn sender()
    }

    if got.Load() != 15 || second.Load() != 2 {
        try io.Printf("got {} then {}\n", got.Load(), second.Load())
        return 1
    }
    try io.Printf("caught {}, then {}\n", nameof(os.Signal.Terminate),
                  nameof(os.Signal.Interrupt))
    return 42
}
