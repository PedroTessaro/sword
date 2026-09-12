// expect: 42
// expect-output: read timed out instead of hanging
// Without a timeout a silent client holds the accept loop for good.

import "std/io"
import "std/net"
import "std/time"

func silent(l *net.Listener, mut ok []u64) !void {
    mut c := try l.Accept()
    try c.SetTimeout(time.Millis(300))
    mut buf := [64]u8{}
    c.Read(buf[..]) catch |e| {
        ok[0] = 1
        c.Close()
        return
    }
    c.Close()
}

func quiet(port i32) !void {
    mut c := try net.Dial("127.0.0.1", port)
    mut wait := [1]u8{}
    c.SetTimeout(time.Seconds(2)) catch {}
    c.Read(wait[..]) catch {}
    c.Close()
}

func main() !int {
    mut room := [1024]u8{}
    mut slot := [1]u64{}
    mut ok := slot[..]
    mut l := try net.Listen(0)
    port := l.Port()
    scope {
        spawn silent(&l, ok)
        spawn quiet(port)
    }
    l.Close()
    if ok[0] != 1 {
        return 1
    }
    try io.Print("read timed out instead of hanging\n")
    return 42
}
