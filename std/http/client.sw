package http

import "std/bytes"
import "std/mem"
import "std/net"
import "std/strings"
import "std/time"
import "std/tls"

// A request that hangs is worse than one that fails, so the package-level
// helpers come with a limit rather than none.
const DefaultConnectTimeout = 10
const DefaultReadTimeout = 30
// Enough to get through the usual `http → https`, `/x → /x/` pair twice over.
// A loop of redirects is a server misconfigured, and following it forever turns
// that into the client's problem.
const DefaultRedirects = 5

error Interrupted = "the kept connection failed part-way through a request"


// The body points into memory taken from the allocator that was passed in, so
// it stays valid until that allocator is reset or freed.
struct ClientResponse {
    Status  u64
    Headers Headers
    Body    []u8
}

// Connect and Read are separate because they fail for different reasons: a
// host that is not there answers within a moment, while a slow reply is the
// server thinking. Either may be zero to wait as long as the kernel would.
//
// A client keeps the last connection it used, so a second call to the same place
// costs no handshake — which over TLS is most of what a request costs. That state
// is why its methods take a `mut` receiver, and why a client belongs to one task:
// two of them sharing one would be a race, and the checker says so.
struct Client {
    Connect time.Duration
    Read    time.Duration
    // How many redirects to follow before giving up. Zero hands the 3xx back as
    // the answer, which is what a client that wants to decide for itself needs.
    Redirects u64
    // How an `https` URL is trusted. Ignored for plain HTTP.
    TLS tls.Config

    // The kept connection, and where it goes. One is enough: a client that talks
    // to one service in a row is the shape this is for, and a pool of them is a
    // different thing with a different name.
    plain  ?net.Conn
    secure ?tls.Conn
    ctx    ?tls.Context
    host   string
    port   i32
}

func NewClient() Client {
    return Client{Connect: time.Seconds(DefaultConnectTimeout),
                  Read: time.Seconds(DefaultReadTimeout),
                  Redirects: DefaultRedirects,
                  TLS: tls.NewConfig(),
                  plain: nil, secure: nil, ctx: nil, host: "", port: 0}
}

// Lets go of the kept connection. A client that is done with should be closed, or
// the socket stays open until the process ends — and over TLS so does the
// certificate store behind it.
func (mut c *Client) Close() {
    if kept := c.plain {
        mut open := kept
        open.Close()
        c.plain = nil
    }
    if kept := c.secure {
        mut open := kept
        open.Close()
        c.secure = nil
    }
    if held := c.ctx {
        mut own := held
        own.Free()
        c.ctx = nil
    }
    c.host = ""
    c.port = 0
}

// Whether the kept connection goes where this request is going.
func (c *Client) kept(host string, port i32, secure bool) bool {
    if c.host != host || c.port != port {
        return false
    }
    if secure {
        return c.secure != nil
    }
    return c.plain != nil
}

// `HTTP/1.1 200 OK`
func parseStatusLine(text string) !u64 {
    first := strings.IndexByte(text, 32)
    if first == text.len {
        return error.BadStatusLine
    }
    rest := text[first+1..text.len]
    // The reason phrase is optional, so the code may run to the end of the
    // line.
    end := strings.IndexByte(rest, 32)
    return try strings.ParseU64(rest[0..end])
}

// A response, and whether the connection it came on can be used again.
struct reply {
    res      ClientResponse
    reusable bool
}

func readResponse(c net.Stream, mut buf []u8) !reply {
    mut have u64 = 0
    mut head u64 = 0
    for {
        if have > 0 {
            head = headerEnd(string(buf[0..have]))
            if head > 0 {
                break
            }
        }
        if have >= buf.len {
            return error.HeadTooLarge
        }
        n := try c.Read(buf[have..buf.len])
        if n == 0 {
            return error.Truncated
        }
        have += n
    }

    mut out := ClientResponse{Status: 0, Headers: NewHeaders(),
                              Body: buf[0..0]}
    block := string(buf[0..head])
    cut := nextLine(block)
    out.Status = try parseStatusLine(cut.text)
    try parseHeaders(cut.rest, &out.Headers)

    // Chunked, a length, or neither. The third is not an error: a reply with no
    // framing at all ends when the connection does, which is how HTTP/1.0
    // answers and how a `Connection: close` reply is allowed to answer.
    keep := !strings.EqualFold(out.Headers.Get("Connection"), "close")

    if isChunked(&out.Headers) {
        got := try readChunked(c, buf, head, have)
        out.Body = buf[head..head+got.body]
        return reply{res: out, reusable: keep}
    }

    length := out.Headers.Get("Content-Length")
    if length.len == 0 {
        for have < buf.len {
            n := c.Read(buf[have..buf.len]) catch 0
            if n == 0 {
                break
            }
            have += n
        }
        out.Body = buf[head..have]
        // No framing at all: the body ended because the connection did, so there
        // is nothing left to reuse.
        return reply{res: out, reusable: false}
    }

    want := try strings.ParseU64(length)
    if head + want > buf.len {
        return error.BodyTooLarge
    }
    for have < head + want {
        n := try c.Read(buf[have..buf.len])
        if n == 0 {
            return error.Truncated
        }
        have += n
    }
    out.Body = buf[head..head+want]
    return reply{res: out, reusable: keep}
}

// A 303 becomes a GET, and so do 301 and 302 in every client that exists — the
// specification says otherwise and the web decided. 307 and 308 keep the method
// and the body, which is what they were added for.
func redirectsAs(status u64, method string) string {
    if status == 307 || status == 308 {
        return method
    }
    return "GET"
}

func followable(status u64) bool {
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308
}

// Sends the request and follows up to `Redirects` of them. Each hop is a fresh
// connection to whatever the Location said, which may be another host.
func (mut c *Client) Do(host string, port i32, method string, target string,
                    contentType string, body []u8,
                    mut a mem.Allocator) !ClientResponse {
    return try c.send(host, port, method, target, contentType, body, false, a)
}

// The same, over TLS, which is what an `https` URL means.
func (mut c *Client) DoTLS(host string, port i32, method string, target string,
                       contentType string, body []u8,
                       mut a mem.Allocator) !ClientResponse {
    return try c.send(host, port, method, target, contentType, body, true, a)
}

func (mut c *Client) send(host string, port i32, method string, target string,
                      contentType string, body []u8, secure bool,
                      mut a mem.Allocator) !ClientResponse {
    mut useHost := host
    mut usePort := port
    mut useMethod := method
    mut useTarget := target
    mut useBody := body
    mut useTLS := secure
    // Out here rather than in the loop: `useBody` outlives an iteration.
    mut empty := [0]u8{}

    mut hop u64 = 0
    for {
        res := try c.once(useHost, usePort, useMethod, useTarget, contentType,
                          useBody, useTLS, a)
        if hop >= c.Redirects || !followable(res.Status) {
            return res
        }
        where := res.Headers.Get("Location")
        if where.len == 0 {
            return res
        }
        next := try ParseURL(where, useHost, usePort)
        useTLS = next.TLS
        useMethod = redirectsAs(res.Status, useMethod)
        if useMethod == "GET" {
            useBody = empty[..]
        }
        useHost = next.Host
        usePort = next.Port
        useTarget = next.Target
        hop += 1
    }
}

// One request, one connection, one answer.
func (mut c *Client) once(host string, port i32, method string, target string,
                          contentType string, body []u8, secure bool,
                          mut a mem.Allocator) !ClientResponse {
    mut req := try bytes.New(a, 512)
    try req.WriteString(method)
    try req.WriteByte(32)
    try req.WriteString(target)
    try req.WriteString(" HTTP/1.1\r\nHost: ")
    try req.WriteString(host)
    try req.WriteString("\r\n")
    if body.len > 0 {
        try req.WriteString("Content-Type: ")
        try req.WriteString(contentType)
        try req.WriteString("\r\nContent-Length: ")
        try req.WriteU64(body.len)
        try req.WriteString("\r\n")
    }
    try req.WriteString("\r\n")
    try req.Write(body)

    mut buf := mem.Alloc[u8](a, MaxHead) orelse return error.OutOfMemory

    // The kept connection first. A socket that has been sitting idle may have
    // been closed by the far end without a word, and using it is the only way to
    // find out — which is why this can come back with nothing and no error.
    if c.kept(host, port, secure) {
        if answer := c.onKept(req.Bytes(), buf, secure) {
            return answer
        }
        c.Close()
        // Retrying is only safe when repeating the request is: a GET or a HEAD
        // asks for something, and anything else may already have happened at the
        // other end even though the answer never arrived.
        if method != "GET" && method != "HEAD" {
            return error.Interrupted
        }
    }

    mut socket := try net.DialTimeout(host, port, c.Connect)
    socket.SetTimeout(c.Read) catch {}

    if !secure {
        socket.Write(req.Bytes()) catch |e| {
            socket.Close()
            return e
        }
        answer := readResponse(&socket, buf) catch |e| {
            socket.Close()
            return e
        }
        if answer.reusable {
            c.plain = socket
            c.host = host
            c.port = port
        } else {
            socket.Close()
        }
        return answer.res
    }

    mut ctx := try tls.ClientContext(c.TLS)
    mut conn := tls.Client(&ctx, socket, host) catch |e| {
        ctx.Free()
        return e
    }
    conn.SetTimeout(c.Read) catch {}
    conn.Write(req.Bytes()) catch |e| {
        conn.Close()
        ctx.Free()
        return e
    }
    answer := readResponse(&conn, buf) catch |e| {
        conn.Close()
        ctx.Free()
        return e
    }
    if answer.reusable {
        // The context is kept with the connection: it is what the session was
        // built against, and freeing it would take the connection with it.
        c.secure = conn
        c.ctx = ctx
        c.host = host
        c.port = port
    } else {
        conn.Close()
        ctx.Free()
    }
    return answer.res
}

// The same request on the connection already open, or nothing if that connection
// turned out to be gone. Nothing is closed here — the caller decides, because it
// knows whether retrying is allowed.
func (mut c *Client) onKept(request []u8, mut buf []u8,
                            secure bool) ?ClientResponse {
    if secure {
        mut conn := c.secure orelse return nil
        conn.Write(request) catch return nil
        answer := readResponse(&conn, buf) catch return nil
        if !answer.reusable {
            conn.Close()
            c.secure = nil
            if held := c.ctx {
                mut own := held
                own.Free()
                c.ctx = nil
            }
            c.host = ""
        }
        return answer.res
    }

    mut socket := c.plain orelse return nil
    socket.Write(request) catch return nil
    answer := readResponse(&socket, buf) catch return nil
    if !answer.reusable {
        socket.Close()
        c.plain = nil
        c.host = ""
    }
    return answer.res
}

func (mut c *Client) Get(host string, port i32, target string,
                     mut a mem.Allocator) !ClientResponse {
    mut empty := [0]u8{}
    return try c.Do(host, port, "GET", target, "", empty[..], a)
}

func (mut c *Client) Post(host string, port i32, target string,
                      contentType string, body []u8,
                      mut a mem.Allocator) !ClientResponse {
    return try c.Do(host, port, "POST", target, contentType, body, a)
}

// The same two over TLS, for a host and port already known — `Fetch` is the one
// to reach for when what you have is a URL.
func (mut c *Client) GetTLS(host string, port i32, target string,
                        mut a mem.Allocator) !ClientResponse {
    mut empty := [0]u8{}
    return try c.DoTLS(host, port, "GET", target, "", empty[..], a)
}

func (mut c *Client) PostTLS(host string, port i32, target string,
                         contentType string, body []u8,
                         mut a mem.Allocator) !ClientResponse {
    return try c.DoTLS(host, port, "POST", target, contentType, body, a)
}

// A whole URL rather than its pieces, which is the only way to say `https` and
// the only shape a redirect comes in. `http://` and `https://` both work; the
// second needs a build with TLS.
//
//     res := try http.Fetch("https://example.com/health", &arena)
func (mut c *Client) Fetch(url string, mut a mem.Allocator) !ClientResponse {
    where := try ParseURL(url, "", 0)
    mut empty := [0]u8{}
    if where.TLS {
        return try c.DoTLS(where.Host, where.Port, "GET", where.Target, "",
                           empty[..], a)
    }
    return try c.Do(where.Host, where.Port, "GET", where.Target, "", empty[..],
                    a)
}

func Fetch(url string, mut a mem.Allocator) !ClientResponse {
    mut client := NewClient()
    defer client.Close()
    return try client.Fetch(url, a)
}

// The same request through a default client, which is what most calls want.
func Get(host string, port i32, target string,
         mut a mem.Allocator) !ClientResponse {
    mut client := NewClient()
    defer client.Close()
    return try client.Get(host, port, target, a)
}

func Post(host string, port i32, target string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse {
    mut client := NewClient()
    defer client.Close()
    return try client.Post(host, port, target, contentType, body, a)
}
