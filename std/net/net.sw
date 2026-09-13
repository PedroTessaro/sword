package net

import "std/time"

// The socket layer is the one place the standard library leans on C: address
// structures are laid out differently on BSD and Linux, and Sword has no
// conditional compilation to tell them apart. Everything above this file is
// written in Sword.
extern func sword_net_listen(port i32, backlog i32) i32
extern func sword_net_listen_on(host [*]u8, host_len i64, port i32,
                                backlog i32, reuse_port i32) i32
extern func sword_net_peer(fd i32, out [*]u8, out_len i64) i32
extern func sword_net_port(fd i32) i32
extern func sword_net_accept(fd i32) i32
extern func sword_net_dial(host [*]u8, host_len i64, port i32) i32
extern func sword_net_dial_timeout(host [*]u8, host_len i64, port i32,
                                   millis i64) i32
extern func sword_net_read(fd i32, buf [*]u8, len i64) i64
extern func sword_net_write(fd i32, buf [*]u8, len i64) i64
extern func sword_net_timeout(fd i32, millis i64) i32
extern func sword_net_deadline(fd i32, at_ns i64) i32
extern func sword_net_close(fd i32) i32
extern func sword_net_stop(fd i32) i32

const DefaultBacklog = 128

// Something bytes go through, with a deadline: a socket, or a socket with TLS
// over it. A server written against this serves either, and the one place that
// knows the difference is where the connection is made.
//
// Satisfied by a pointer, like every interface here, so the value it points at
// has to outlive the borrow — a task that owns its connection passes `&conn` and
// that is exactly right.
interface Stream {
    Read(mut into []u8) !u64
    Write(from []u8) !void
    WriteString(s string) !void
    SetTimeout(limit time.Duration) !void
    SetDeadline(at time.Instant) !void
    Peer(mut into []u8) !Peer
    Close()
}

// A socket is just its descriptor. Copying one copies the number, not the
// connection, so close it only where it is owned.
struct Conn {
    fd i32
}

struct Listener {
    fd i32
}

// Loopback only: nothing off this machine can reach it. Port 0 asks the
// operating system to pick one; ask the listener afterwards which it got.
func Listen(port i32) !Listener {
    fd := sword_net_listen(port, DefaultBacklog)
    if fd < 0 {
        return error.ListenFailed
    }
    return Listener{fd: fd}
}

// The address to bind, empty meaning every interface — which is what a server
// that is meant to be reached needs.
//
// `share` lets other sockets hold the same port and has the kernel spread
// connections between them, which is how one process is replaced by another
// without dropping anything in between.
func ListenOn(host string, port i32, share bool) !Listener {
    mut shared i32 = 0
    if share {
        shared = 1
    }
    fd := sword_net_listen_on(host.ptr, i64(host.len), port, DefaultBacklog,
                              shared)
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

// The kernel gives up on a connection after a minute or more, which is far
// too long for anything a person is waiting on. This bounds it.
func DialTimeout(host string, port i32, limit time.Duration) !Conn {
    fd := sword_net_dial_timeout(host.ptr, i64(host.len), port, limit.AsMillis())
    if fd == -2 {
        return error.Timeout
    }
    if fd < 0 {
        return error.DialFailed
    }
    return Conn{fd: fd}
}

// The most any single wait on this connection may take. A zero duration waits
// as long as it takes, which is what a socket does by default.
// The descriptor underneath, for a layer that has to speak to the socket itself
// rather than through this one — `std/tls` hands it to OpenSSL. Whoever takes it
// does not own it: the connection still closes it.
func (c *Conn) Fd() i32 {
    return c.fd
}

func (c *Conn) SetTimeout(limit time.Duration) !void {
    if sword_net_timeout(c.fd, limit.AsMillis()) < 0 {
        return error.TimeoutNotSet
    }
}

// A point after which nothing on this connection waits any longer, however
// little each wait took on its own. A timeout cannot bound a client that sends
// one byte a second forever; this can.
//
//     try c.SetDeadline(time.Now().Add(time.Seconds(30)))
//
// The zero instant clears it.
func (c *Conn) SetDeadline(at time.Instant) !void {
    if sword_net_deadline(c.fd, at.AsNanos()) < 0 {
        return error.TimeoutNotSet
    }
}

func (c *Conn) ClearDeadline() !void {
    if sword_net_deadline(c.fd, 0) < 0 {
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

// Who is on the other end. The address is written into space the caller gives,
// because a connection has no allocator of its own.
struct Peer {
    Address string
    Port    i32
}

func (c *Conn) Peer(mut into []u8) !Peer {
    port := sword_net_peer(c.fd, into.ptr, i64(into.len))
    if port < 0 {
        return error.NoPeer
    }
    mut n u64 = 0
    for n < into.len && into[n] != 0 {
        n += 1
    }
    return Peer{Address: string(into[0..n]), Port: port}
}

func (c *Conn) Close() {
    sword_net_close(c.fd)
}
