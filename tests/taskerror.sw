// expect: 42
// expect-output: propagou

import "std/io"

func work(id i64, fail bool) !void {
    if fail {
        return error.TaskFailed
    }
}

func run(bad bool) !void {
    scope {
        for i in 0..8 {
            spawn work(i, bad && i == 5)
        }
    }
}

func main() !int {
    run(false) catch return 90

    run(true) catch |e| {
        try io.Print("propagou\n")
        return 42
    }
    return 91
}
