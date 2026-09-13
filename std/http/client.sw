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
struct Client {
    Connect time.Duration
    Read    time.Duration
    // How many redirects to follow before giving up. Zero hands the 3xx back as
    // the answer, which is what a client that wants to decide for itself needs.
    Redirects u64
    // How an `https` URL is trusted. Ignored for plain HTTP.
    TLS tls.Config
}

func NewClient() Client {
    return Client{Connect: time.Seconds(DefaultConnectTimeout),
                  Read: time.Seconds(DefaultReadTimeout),
                  Redirects: DefaultRedirects,
                  TLS: tls.NewConfig()}
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

func readResponse(c net.Stream, mut buf []u8) !ClientResponse {
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
    if isChunked(&out.Headers) {
        got := try readChunked(c, buf, head, have)
        out.Body = buf[head..head+got.body]
        return out
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
        return out
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
    return out
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
func (c *Client) Do(host string, port i32, method string, target string,
                    contentType string, body []u8,
                    mut a mem.Allocator) !ClientResponse {
    return try c.send(host, port, method, target, contentType, body, false, a)
}

// The same, over TLS, which is what an `https` URL means.
func (c *Client) DoTLS(host string, port i32, method string, target string,
                       contentType string, body []u8,
                       mut a mem.Allocator) !ClientResponse {
    return try c.send(host, port, method, target, contentType, body, true, a)
}

func (c *Client) send(host string, port i32, method string, target string,
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
func (c *Client) once(host string, port i32, method string, target string,
                      contentType string, body []u8, secure bool,
                      mut a mem.Allocator) !ClientResponse {
    mut req := try bytes.New(a, 512)
    try req.WriteString(method)
    try req.WriteByte(32)
    try req.WriteString(target)
    try req.WriteString(" HTTP/1.1\r\nHost: ")
    try req.WriteString(host)
    try req.WriteString("\r\nConnection: close\r\n")
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
    mut socket := try net.DialTimeout(host, port, c.Connect)
    socket.SetTimeout(c.Read) catch {}

    if !secure {
        socket.Write(req.Bytes()) catch |e| {
            socket.Close()
            return e
        }
        res := readResponse(&socket, buf) catch |e| {
            socket.Close()
            return e
        }
        socket.Close()
        return res
    }

    // A context per request: this client keeps nothing between calls, and one
    // shared across tasks would be state two of them could reach.
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
    res := readResponse(&conn, buf) catch |e| {
        conn.Close()
        ctx.Free()
        return e
    }
    conn.Close()
    ctx.Free()
    return res
}

func (c *Client) Get(host string, port i32, target string,
                     mut a mem.Allocator) !ClientResponse {
    mut empty := [0]u8{}
    return try c.Do(host, port, "GET", target, "", empty[..], a)
}

func (c *Client) Post(host string, port i32, target string,
                      contentType string, body []u8,
                      mut a mem.Allocator) !ClientResponse {
    return try c.Do(host, port, "POST", target, contentType, body, a)
}

// The same two over TLS, for a host and port already known — `Fetch` is the one
// to reach for when what you have is a URL.
func (c *Client) GetTLS(host string, port i32, target string,
                        mut a mem.Allocator) !ClientResponse {
    mut empty := [0]u8{}
    return try c.DoTLS(host, port, "GET", target, "", empty[..], a)
}

func (c *Client) PostTLS(host string, port i32, target string,
                         contentType string, body []u8,
                         mut a mem.Allocator) !ClientResponse {
    return try c.DoTLS(host, port, "POST", target, contentType, body, a)
}

// A whole URL rather than its pieces, which is the only way to say `https` and
// the only shape a redirect comes in. `http://` and `https://` both work; the
// second needs a build with TLS.
//
//     res := try http.Fetch("https://example.com/health", &arena)
func (c *Client) Fetch(url string, mut a mem.Allocator) !ClientResponse {
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
    client := NewClient()
    return try client.Fetch(url, a)
}

// The same request through a default client, which is what most calls want.
func Get(host string, port i32, target string,
         mut a mem.Allocator) !ClientResponse {
    client := NewClient()
    return try client.Get(host, port, target, a)
}

func Post(host string, port i32, target string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse {
    client := NewClient()
    return try client.Post(host, port, target, contentType, body, a)
}
