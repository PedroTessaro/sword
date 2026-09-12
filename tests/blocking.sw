// expect: 42
// expect-output: 48 tasks blocked at once
// Far more tasks park in a socket read than the machine has cores. Every one
// of them holds a thread, so this only finishes if the pool grows to cover
// them; on a fixed pool it stops here.

import "std/io"
import "std/net"
import "std/time"

const Waiters = 48

func waiter(port i32, mut ready *atomic[u64], mut freed *atomic[u64]) !void {
    mut c := try net.Dial("127.0.0.1", port)
    ready.Add(1)
    mut one := [1]u8{}
    n := c.Read(one[..]) catch 0
    if n == 1 {
        freed.Add(1)
    }
    c.Close()
}

func gate(l *net.Listener, mut ready *atomic[u64]) !void {
    mut conns := [Waiters]net.Conn{}
    for i in 0..Waiters {
        conns[i] = try l.Accept()
    }
    // Nobody is released until everybody has arrived, which is what forces
    // them to be blocked at the same moment rather than one after another.
    for ready.Load() < Waiters {
        time.Sleep(time.Millis(1))
    }
    for i in 0..Waiters {
        conns[i].WriteString("x") catch {}
        conns[i].Close()
    }
}

func main() !int {
    mut ready := atomic[u64](0)
    mut freed := atomic[u64](0)

    mut l := try net.Listen(0)
    port := l.Port()
    scope {
        spawn gate(&l, &ready)
        for i in 0..Waiters {
            spawn waiter(port, &ready, &freed)
        }
    }
    l.Close()

    if freed.Load() != Waiters {
        return 1
    }
    try io.Printf("{} tasks blocked at once\n", freed.Load())
    return 42
}
