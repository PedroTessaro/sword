package http

import "std/bytes"
import "std/fmt"
import "std/mem"
import "std/net"
import "std/strings"
import "std/time"

// A connection's arena. One per connection rather than one per worker: a task
// waiting on a socket is put down rather than holding a thread, so connections
// outnumber threads by a lot and each needs its own memory.
const ArenaSize = 32768
// Most connections at once. Past this the server stops accepting and lets the
// kernel's backlog hold the rest, which is what backpressure looks like from
// the outside: a queue rather than a collapse.
const DefaultMaxConns = 1024
// A client that connects and then says nothing releases its connection after
// this long.
const DefaultTimeout = 15

const MaxParams = 8

struct Param {
    Name, Value string
}

struct Request {
    // Target is the request line unchanged; Path and RawQuery are its two
    // halves, split at `?`.
    Method, Target, Path, RawQuery, Proto string
    Headers Headers
    Body    []u8
    params  [MaxParams]Param
    pcount  u64
}

// The segment a `{name}` in the route pattern matched, empty when the request
// did not come through a Mux or the pattern had no such wildcard.
func (r *Request) Param(name string) string {
    for i in 0..r.pcount {
        if r.params[i].Name == name {
            return r.params[i].Value
        }
    }
    return ""
}

// Still percent-encoded: decoding needs somewhere to put the result, and the
// request has no allocator. Pass it through Unescape when the value can carry
// more than letters and digits.
func (r *Request) Query(name string) string {
    return QueryValue(r.RawQuery, name)
}

struct Response {
    Status  u64
    Headers Headers
    body    bytes.Buffer
}

// A handler is an interface rather than a function value, the same way Go's
// http.Handler is: it gives the handler somewhere to keep its own state.
interface Handler {
    Serve(req *Request, mut res *Response) !void
}

func NewHeaders() Headers {
    return Headers{items: [MaxHeaders]Header{}, count: 0}
}

// The body starts as an empty window into the read buffer, so it always points
// at memory that is alive.
func newRequest(buf []u8) Request {
    return Request{Method: "", Target: "", Path: "", RawQuery: "", Proto: "",
                   Headers: NewHeaders(), Body: buf[0..0],
                   params: [MaxParams]Param{}, pcount: 0}
}

func (mut r *Response) SetHeader(name string, value string) !void {
    try r.Headers.Set(name, value)
}

func (mut r *Response) Write(p []u8) !void {
    try r.body.Write(p)
}

func (mut r *Response) WriteString(s string) !void {
    try r.body.WriteString(s)
}

// Formats straight into the body; `{}` takes the next argument.
func (mut r *Response) Printf(format string, args ...any) !void {
    try fmt.Format(&r.body, format, args...)
}

func (mut r *Response) Text(status u64, s string) !void {
    r.Status = status
    try r.Headers.Set("Content-Type", "text/plain; charset=utf-8")
    try r.body.WriteString(s)
}

func (mut r *Response) JSON(status u64, s string) !void {
    r.Status = status
    try r.Headers.Set("Content-Type", "application/json")
    try r.body.WriteString(s)
}

// `METHOD SP TARGET SP PROTO`
func parseRequestLine(text string, mut req *Request) !void {
    first := strings.IndexByte(text, 32)
    if first == text.len {
        return error.BadRequestLine
    }
    rest := text[first+1..text.len]
    second := strings.IndexByte(rest, 32)
    if second == rest.len {
        return error.BadRequestLine
    }
    req.Method = text[0..first]
    req.Target = rest[0..second]
    req.Proto = rest[second+1..rest.len]

    split := ParseTarget(req.Target)
    req.Path = split.Path
    req.RawQuery = split.RawQuery
}

// Returns how many bytes of `buf` the request occupies, or zero when the peer
// closed before sending anything.
func readRequest(mut c *net.Conn, mut buf []u8, mut req *Request) !u64 {
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
            if have == 0 {
                return 0
            }
            return error.Truncated
        }
        have += n
    }

    block := string(buf[0..head])
    cut := nextLine(block)
    try parseRequestLine(cut.text, req)
    try parseHeaders(cut.rest, &req.Headers)

    want := try bodyLength(&req.Headers)
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
    req.Body = buf[head..head+want]
    return head + want
}

// HTTP/1.1 keeps the connection open unless told otherwise; HTTP/1.0 is the
// other way round.
func wantsKeepAlive(req *Request) bool {
    connection := req.Headers.Get("Connection")
    if strings.EqualFold(connection, "close") {
        return false
    }
    if strings.EqualFold(connection, "keep-alive") {
        return true
    }
    return req.Proto == "HTTP/1.1"
}

func writeResponse(mut c *net.Conn, mut res *Response, keep bool,
                   mut out *bytes.Buffer) !void {
    out.Reset()
    try out.WriteString("HTTP/1.1 ")
    try out.WriteU64(res.Status)
    try out.WriteByte(32)
    try out.WriteString(StatusText(res.Status))
    try out.WriteString("\r\n")

    try out.WriteString("Content-Length: ")
    try out.WriteU64(res.body.Len())
    try out.WriteString("\r\n")

    if keep {
        try out.WriteString("Connection: keep-alive\r\n")
    } else {
        try out.WriteString("Connection: close\r\n")
    }

    try writeHeaders(out, &res.Headers)
    try out.WriteString("\r\n")
    try out.Write(res.body.Bytes())
    try c.Write(out.Bytes())
}

func handleConn(mut c *net.Conn, h Handler, mut a mem.Allocator) !void {
    mut buf := mem.Alloc[u8](a, MaxHead) orelse return error.OutOfMemory
    mut out := try bytes.New(a, 1024)
    mut body := try bytes.New(a, 1024)

    for {
        mut req := newRequest(buf)
        got := try readRequest(c, buf, &req)
        if got == 0 {
            return
        }

        body.Reset()
        mut res := Response{Status: 200, Headers: NewHeaders(), body: body}
        h.Serve(&req, &res) catch {
            res.Status = 500
        }
        body = res.body

        keep := wantsKeepAlive(&req)
        try writeResponse(c, &res, keep, &out)
        if !keep {
            return
        }
    }
}

struct Server {
    listener net.Listener
    // How many connections are being served right now. An atomic because every
    // connection is its own task and they all count themselves.
    live atomic[u64]
}

func Listen(port i32) !Server {
    return Server{listener: try net.Listen(port), live: atomic[u64](0)}
}

func (s *Server) Live() u64 {
    return s.live.Load()
}

// Useful when the port was left to the operating system to choose.
func (s *Server) Port() i32 {
    return s.listener.Port()
}

// Closing the listener is how a running server is stopped: every accept loop
// fails and returns, and the scope around them joins.
func (s *Server) Close() {
    s.listener.Close()
}

// One task per connection, with its own arena on its own stack. That is only
// affordable because a task waiting on a socket costs a stack and not a thread:
// it is put down, and the worker goes to whoever has data.
func serveConn(c net.Conn, h Handler, s *Server) !void {
    mut conn := c
    mut backing := [ArenaSize]u8{}
    mut arena := mem.NewArena(backing[..])
    conn.SetTimeout(time.Seconds(DefaultTimeout)) catch {}
    handleConn(&conn, h, &arena) catch {}
    conn.Close()
    s.live.Sub(1)
}

func (s *Server) Serve(h Handler) !void {
    try s.ServeWith(h, DefaultMaxConns)
}

// The accept loop is the body of the scope rather than a task in it, because
// only the body may spawn. Closing the listener ends it, and then the scope
// waits for every connection still being served — which is what makes shutting
// down graceful without anything extra.
func (s *Server) ServeWith(h Handler, most u64) !void {
    scope {
        for {
            // At the limit the server stops taking work. The backlog holds the
            // rest, and a client that cannot wait gives up on its own.
            for most > 0 && s.live.Load() >= most {
                time.Sleep(time.Millis(1))
            }
            c := s.listener.Accept() catch break
            s.live.Add(1)
            spawn serveConn(c, h, s)
        }
    }
}
