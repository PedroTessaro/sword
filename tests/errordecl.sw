// expect: 42
// expect-output: errors say what they are
// An error used to be conjured by naming it, which meant a typo was a second
// error nobody handled. Now it is declared, and the declaration is where the
// sentence it says lives — in the binary, so raising one allocates nothing.

import "std/io"
import "std/strings"

error Timeout = "the peer did not answer in time"
error Refused = "nothing is listening there"
error quiet                      // declared without a message, on purpose

func fail(which u64) !u64 {
    if which == 0 {
        return error.Timeout
    }
    if which == 1 {
        return error.Refused
    }
    if which == 2 {
        return error.quiet
    }
    return which
}

func main() !int {
    mut seen u64 = 0

    fail(0) catch |e| {
        if nameof(e) == "Timeout" && e.Message() == "the peer did not answer in time" {
            seen += 1
        }
    }
    fail(1) catch |e| {
        if nameof(e) == "Refused" && strings.Contains(e.Message(), "listening") {
            seen += 2
        }
    }
    // No message is an empty message, not a made-up one.
    fail(2) catch |e| {
        if nameof(e) == "quiet" && e.Message().len == 0 {
            seen += 4
        }
    }
    // A code that is not an error has neither.
    if try fail(9) != 9 {
        return 1
    }

    // An error another package declared, carrying that package's words.
    strings.ParseU64("twelve") catch |e| {
        if nameof(e) == "NotANumber" && e.Message().len > 0 {
            seen += 8
        }
    }

    if seen != 15 {
        try io.Printf("seen {}\n", seen)
        return 3
    }
    try io.Print("errors say what they are\n")
    return 42
}
