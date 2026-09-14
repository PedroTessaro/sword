// expect: 134
// A task that runs out of stack should say so and stop, not die with a fault
// address and leave whoever reads the log to guess. 134 is what a shell reports
// for a process that gave up on purpose.

import "std/io"

// Four kilobytes a frame, written to so the compiler cannot drop it. A hundred
// thousand of those is far past any stack the runtime hands out.
func deep(n u64) u64 {
    mut pad := [4096]u8{}
    pad[0] = u8(n % 251)
    pad[4095] = pad[0]
    if n == 0 {
        return u64(pad[0])
    }
    return deep(n - 1) + u64(pad[4095])
}

func run(mut out *atomic[u64]) !void {
    out.Store(deep(100000))
}

func main() !int {
    mut out := atomic[u64](0)
    scope {
        spawn run(&out)
    }
    // Never: the task above takes the process down first.
    try io.Printf("came back with {}\n", out.Load())
    return 0
}
