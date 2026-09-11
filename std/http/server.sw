package http

import "std/bytes"
import "std/fmt"
import "std/mem"
import "std/net"
import "std/strings"

// A worker's arena. Each accept loop has its own, so no allocator is ever
// shared between tasks — which the race checker would reject anyway.
const ArenaSize = 262144
const DefaultWorkers = 4
// A client that connects and then says nothing would otherwise hold an accept
// loop for good; four such clients would be the whole server.
const DefaultTimeout = 15000

struct Request {
    Method, Path, Proto string
    Headers Headers
    Body    []u8
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
    return Request{Method: "", Path: "", Proto: "", Headers: NewHeaders(),
                   Body: buf[0..0]}
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

// `METHOD SP PATH SP PROTO`
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
    req.Path = rest[0..second]
    req.Proto = rest[second+1..rest.len]
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
    return strings.Equal(req.Proto, "HTTP/1.1")
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
}

func Listen(port i32) !Server {
    return Server{listener: try net.Listen(port)}
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

// One task per accept loop, each with its own arena, all inside a scope that
// outlives none of them. The kernel spreads incoming connections across them.
func acceptLoop(s *Server, h Handler) !void {
    mut backing := [ArenaSize]u8{}
    mut arena := mem.NewArena(backing[..])
    for {
        mut c := s.listener.Accept() catch return
        c.SetTimeout(DefaultTimeout) catch {}
        handleConn(&c, h, &arena) catch {}
        c.Close()
        arena.Reset()
    }
}

func (s *Server) Serve(h Handler) !void {
    try s.ServeWith(h, DefaultWorkers)
}

func (s *Server) ServeWith(h Handler, workers int) !void {
    scope {
        for i in 0..workers {
            spawn acceptLoop(s, h)
        }
    }
}
