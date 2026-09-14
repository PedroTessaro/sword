// expect-error: is already declared with a different message
// Two packages may declare the same error — that is what makes `error.Timeout`
// one error across a program, handled once. What they may not do is disagree
// about what it says, because only one of the two sentences could ever be told.

import "std/net"

error Timeout = "something else entirely"

func main() !int {
    mut l := net.Listen(0) catch return 1
    l.Close()
    return 0
}
