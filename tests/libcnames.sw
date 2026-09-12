// expect: 42
// expect-output: libc names are not hijacked
// The program's own package is qualified in the object file for exactly this
// reason. Without it a function called `shutdown` here became the one the
// runtime calls when closing a listener, and the server segfaulted on its way
// out — a long way from anything that looked like the cause.

import "std/fs"
import "std/io"
import "std/net"
import "std/os"

// Every one of these is a libc symbol the runtime itself calls.
func shutdown(n u64) u64 {
    return n + 1
}

func read(n u64) u64 {
    return n + 2
}

func write(n u64) u64 {
    return n + 4
}

func close(n u64) u64 {
    return n + 8
}

func open(n u64) u64 {
    return n + 16
}

func poll(n u64) u64 {
    return n + 32
}

func kill(n u64) u64 {
    return n + 64
}

func signal(n u64) u64 {
    return n + 128
}

func main() !int {
    // Ours do what they say.
    mut sum u64 = 0
    sum += shutdown(0) + read(0) + write(0) + close(0)
    sum += open(0) + poll(0) + kill(0) + signal(0)
    if sum != 255 {
        return 1
    }

    // And the runtime still reaches libc's. A listener opened and shut down is
    // the exact path that broke: shutdown, then close.
    mut l := try net.Listen(0)
    if l.Port() <= 0 {
        return 2
    }
    l.Close()

    // Files go through open, read, write and close as well.
    path := "/tmp/sword_libcnames.txt"
    try fs.WriteAll(path, []u8("ok"))
    back := fs.Size(path) orelse return 3
    if back != 2 {
        return 4
    }
    try fs.Remove(path)

    if os.Pid() == 0 {
        return 5
    }

    try io.Print("libc names are not hijacked\n")
    return 42
}
