// expect: 42
// expect-output: os and time work
// Arguments, environment and both clocks. The test runner passes no arguments,
// so argc is one: the program's own name.

import "std/io"
import "std/os"
import "std/time"

func main() !int {
    mut room := [8]string{}
    args := os.Args(room[..])
    if args.len != os.Argc() || args.len < 1 {
        return 1
    }
    if args[0].len == 0 {
        return 2
    }
    // Past the end is empty rather than an error, so a missing argument does
    // not have to be unwrapped.
    if os.Arg(args.len + 10).len != 0 {
        return 3
    }

    if found := os.Env("SWORD_DEFINITELY_NOT_SET") {
        return 4
    }
    if os.EnvOr("SWORD_DEFINITELY_NOT_SET", "fallback").len != 8 {
        return 5
    }

    start := time.Now()
    time.Sleep(time.Millis(40))
    took := time.Since(start)
    if took.AsMillis() < 35 {
        return 6
    }
    if took.AsSeconds() > 5.0 {
        return 7
    }

    // Comfortably after 2020 and before the clock runs out of i64 nanoseconds.
    if time.Unix() < 1577836800 {
        return 8
    }

    short := time.Millis(5)
    long := time.Seconds(1)
    if !short.Less(long) || long.Less(short) {
        return 9
    }
    total := short.Add(long)
    if total.AsMillis() != 1005 {
        return 10
    }

    try io.Printf("os and time work, slept {}ms\n", took.AsMillis())
    return 42
}
