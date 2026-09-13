package tls

import "std/net"
import "std/time"

extern func sword_tls_available() i32
extern func sword_tls_client_context(ca [*]u8, ca_len i64, verify i32) ?[*]u8
extern func sword_tls_server_context(cert [*]u8, cert_len i64, key [*]u8,
                                     key_len i64) ?[*]u8
extern func sword_tls_free_context(ctx [*]u8)
extern func sword_tls_connect(ctx [*]u8, fd i32, host [*]u8,
                              host_len i64) ?[*]u8
extern func sword_tls_accept(ctx [*]u8, fd i32) ?[*]u8
extern func sword_tls_read(conn [*]u8, buf [*]u8, len i64) i64
extern func sword_tls_write(conn [*]u8, buf [*]u8, len i64) i64
extern func sword_tls_close(conn [*]u8)
extern func sword_tls_version(conn [*]u8, len *i64) [*]u8
extern func sword_tls_cipher(conn [*]u8, len *i64) [*]u8
extern func sword_tls_self_signed(host [*]u8, host_len i64, cert [*]u8,
                                  cert_len i64, key [*]u8, key_len i64) i32

// Whether this build can speak TLS at all. The protocol is not something to
// write by hand, so it comes from OpenSSL, and a machine that had no OpenSSL
// when the compiler was built gets a compiler without it. Everything here fails
// with error.NoTLS in that case, and nothing else in the language is affected.
func Available() bool {
    return sword_tls_available() == 1
}

// Where the trust comes from and whether it is checked at all. `Verify` off is
// for a development server holding a certificate nobody signed — it turns TLS
// into encryption without identity, which is not the same thing as security.
struct Config {
    // A PEM file of certificate authorities. Empty uses the system's.
    CAFile string
    Verify bool
}

func NewConfig() Config {
    return Config{CAFile: "", Verify: true}
}

// One certificate, one key and the trust settings, shared by any number of
// connections. Making one parses the certificate, so a server makes it once and
// hands it to every task rather than per connection.
struct Context {
    handle [*]u8
}

func (c *Context) Free() {
    sword_tls_free_context(c.handle)
}

// A connection carrying the socket it took over. Reads and writes go through the
// protocol; everything else — deadlines, the peer's address, closing — is the
// socket's, which is why it stays reachable.
struct Conn {
    Socket net.Conn
    handle [*]u8
}

func ClientContext(c Config) !Context {
    if !Available() {
        return error.NoTLS
    }
    mut verify i32 = 0
    if c.Verify {
        verify = 1
    }
    handle := sword_tls_client_context(c.CAFile.ptr, i64(c.CAFile.len),
                                      verify) orelse return error.BadTrustStore
    return Context{handle: handle}
}

// The certificate chain and its private key, both PEM. A chain rather than one
// certificate: everything but a self-signed one needs the intermediates, and a
// client that cannot build the chain refuses the connection.
func ServerContext(certFile string, keyFile string) !Context {
    if !Available() {
        return error.NoTLS
    }
    handle := sword_tls_server_context(certFile.ptr, i64(certFile.len),
                                       keyFile.ptr, i64(keyFile.len))
    return Context{handle: handle orelse return error.BadCertificate}
}

// Dials and handshakes. The host is checked against the certificate and sent as
// SNI, so it has to be the name the server answers to and not an address —
// unless the certificate says otherwise, which is what a self-signed one for
// `127.0.0.1` does.
func Dial(ctx *Context, host string, port i32) !Conn {
    mut socket := try net.Dial(host, port)
    got := sword_tls_connect(ctx.handle, socket.Fd(), host.ptr, i64(host.len))
    handle := got orelse {
        socket.Close()
        return error.HandshakeFailed
    }
    return Conn{Socket: socket, handle: handle}
}

// The same over a socket somebody else connected, for a client that needed to do
// something to the socket first. The socket becomes the connection's.
func Client(ctx *Context, socket net.Conn, host string) !Conn {
    mut taken := socket
    got := sword_tls_connect(ctx.handle, taken.Fd(), host.ptr, i64(host.len))
    handle := got orelse {
        taken.Close()
        return error.HandshakeFailed
    }
    return Conn{Socket: taken, handle: handle}
}

// The server's half: an accepted socket, handshaked. A handshake that fails is
// one connection lost, not a server down, which is why this returns an error
// rather than taking the accept loop with it.
func Server(ctx *Context, socket net.Conn) !Conn {
    mut taken := socket
    got := sword_tls_accept(ctx.handle, taken.Fd())
    handle := got orelse {
        taken.Close()
        return error.HandshakeFailed
    }
    return Conn{Socket: taken, handle: handle}
}

// Bytes read, which may be fewer than there is room for. Zero means the peer
// closed the connection properly, which over TLS is a message and not a guess.
func (c *Conn) Read(mut into []u8) !u64 {
    if into.len == 0 {
        return 0
    }
    n := sword_tls_read(c.handle, into.ptr, i64(into.len))
    if n == -2 {
        return error.Timeout
    }
    if n < 0 {
        return error.ReadFailed
    }
    return u64(n)
}

// All of it or an error, the same as a socket write.
func (c *Conn) Write(from []u8) !void {
    if from.len == 0 {
        return
    }
    n := sword_tls_write(c.handle, from.ptr, i64(from.len))
    if n == -2 {
        return error.Timeout
    }
    if n < 0 || u64(n) != from.len {
        return error.WriteFailed
    }
}

func (c *Conn) WriteString(s string) !void {
    try c.Write([]u8(s))
}

// The close notification, then the socket. Nothing waits for the peer's half of
// it: that would hold the task on a connection that may never answer.
func (c *Conn) Close() {
    sword_tls_close(c.handle)
    c.Socket.Close()
}

func (c *Conn) SetTimeout(limit time.Duration) !void {
    try c.Socket.SetTimeout(limit)
}

// Who is on the other end, which is the socket's business rather than the
// protocol's. Here so that a `tls.Conn` is a `net.Stream`.
func (c *Conn) Peer(mut into []u8) !net.Peer {
    return try c.Socket.Peer(into)
}

func (c *Conn) SetDeadline(at time.Instant) !void {
    try c.Socket.SetDeadline(at)
}

// What was agreed, for a log line or a header: "TLSv1.3" and the cipher suite.
func (c *Conn) Version() string {
    mut len i64 = 0
    text := sword_tls_version(c.handle, &len)
    return string(text[0..u64(len)])
}

func (c *Conn) Cipher() string {
    mut len i64 = 0
    text := sword_tls_cipher(c.handle, &len)
    return string(text[0..u64(len)])
}

// A certificate for `host` signed by its own key, written to two files, valid for
// a day. For working on your own machine and for tests. A client will refuse it
// unless it is told to trust it or told not to verify, which is exactly right: it
// proves nothing about who is on the other end.
func SelfSigned(host string, certFile string, keyFile string) !void {
    if !Available() {
        return error.NoTLS
    }
    if sword_tls_self_signed(host.ptr, i64(host.len), certFile.ptr,
                             i64(certFile.len), keyFile.ptr,
                             i64(keyFile.len)) != 0 {
        return error.CannotWriteCertificate
    }
}
