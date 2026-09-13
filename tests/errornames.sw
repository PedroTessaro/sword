// expect: 42
// expect-output: OutOfMemory Timeout NotFound
// An error is a code at run time, so its name has to come from a table the
// compiler writes. Same machinery as an enum's names, built during lowering
// because the set is only complete once every package has been checked.

import "std/io"

error NotFound = "there is nothing there"
error Timeout  = "the wait ran out of time"

func fails(which u64) !u64 {
    switch which {
    case 0:
        return error.OutOfMemory
    case 1:
        return error.Timeout
    case 2:
        return error.NotFound
    default:
        return 7
    }
}

func main() !int {
    mut seen := [3]string{}
    for i in 0..3 {
        fails(u64(i)) catch |e| {
            seen[i] = nameof(e)
        }
    }
    try io.Printf("{} {} {}\n", seen[0], seen[1], seen[2])
    if seen[0] != "OutOfMemory" || seen[1] != "Timeout" || seen[2] != "NotFound" {
        return 1
    }

    // Named directly, and success has no name because it is not an error.
    if nameof(error.Timeout) != "Timeout" {
        return 2
    }
    mut none error = 0
    if nameof(none) != "" {
        return 3
    }
    return 42
}
