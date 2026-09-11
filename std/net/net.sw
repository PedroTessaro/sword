package net

// The socket layer is the one place the standard library leans on C: address
// structures are laid out differently on BSD and Linux, and Sword has no
// conditional compilation to tell them apart. Everything above this file is
// written in Sword.
extern func sword_net_listen(port i32, backlog i32) i32
extern func sword_net_port(fd i32) i32
extern func sword_net_accept(fd i32) i32
extern func sword_net_dial(host [*]u8, host_len i64, port i32) i32
extern func sword_net_read(fd i32, buf [*]u8, len i64) i64
extern func sword_net_write(fd i32, buf [*]u8, len i64) i64
extern func sword_net_timeout(fd i32, millis i64) i32
extern func sword_net_close(fd i32) i32
extern func sword_net_stop(fd i32) i32

const DefaultBacklog = 128

// A socket is just its descriptor. Copying one copies the number, not the
// connection, so close it only where it is owned.
struct Conn {
    fd i32
}

struct Listener {
    fd i32
}

// Port 0 asks the operating system to pick one; ask the listener afterwards
// which it got.
func Listen(port i32) !Listener {
    fd := sword_net_listen(port, DefaultBacklog)
    if fd < 0 {
        return error.ListenFailed
    }
    return Listener{fd: fd}
}

func (l *Listener) Port() i32 {
    return sword_net_port(l.fd)
}

func (l *Listener) Accept() !Conn {
    fd := sword_net_accept(l.fd)
    if fd < 0 {
        return error.AcceptFailed
    }
    return Conn{fd: fd}
}

// Wakes anything blocked in Accept, which is how a running server is stopped.
func (l *Listener) Close() {
    sword_net_stop(l.fd)
}

func Dial(host string, port i32) !Conn {
    fd := sword_net_dial(host.ptr, i64(host.len), port)
    if fd < 0 {
        return error.DialFailed
    }
    return Conn{fd: fd}
}

// A connection that goes quiet should not hold a worker forever. Zero waits
// indefinitely, which is the default a socket comes with.
func (c *Conn) SetTimeout(millis i64) !void {
    if sword_net_timeout(c.fd, millis) < 0 {
        return error.TimeoutNotSet
    }
}

// How many bytes landed in `into`; zero means the peer is done sending.
func (c *Conn) Read(mut into []u8) !u64 {
    n := sword_net_read(c.fd, into.ptr, i64(into.len))
    if n == -2 {
        return error.Timeout
    }
    if n < 0 {
        return error.ReadFailed
    }
    return u64(n)
}

func (c *Conn) Write(from []u8) !void {
    if from.len == 0 {
        return
    }
    n := sword_net_write(c.fd, from.ptr, i64(from.len))
    if n == -2 {
        return error.Timeout
    }
    if n < 0 {
        return error.WriteFailed
    }
}

func (c *Conn) WriteString(s string) !void {
    try c.Write([]u8(s))
}

func (c *Conn) Close() {
    sword_net_close(c.fd)
}
