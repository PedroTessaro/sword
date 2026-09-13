package http

import "std/bytes"
import "std/fmt"
import "std/mem"
import "std/net"
import "std/strings"
import "std/time"

// A connection's arena, on its own stack. One per connection rather than one
// per worker: a task waiting on a socket is put down rather than holding a
// thread, so connections outnumber threads by a lot and each needs its own.
//
// Its size is most of what an idle connection costs, and the cost comes in
// whole pages: 32 KiB here measured 49 KiB per idle connection, 16 KiB measures
// 33 KiB. Pooling the arena instead would save one more page, at the price of a
// lock on the hottest path in the server.
const ArenaSize = 16384
// Most connections at once. Past this the server stops accepting and lets the
// kernel's backlog hold the rest, which is what backpressure looks like from
// the outside: a queue rather than a collapse.
const DefaultMaxConns = 1024
// The most any single wait on a connection may take: a client that connects and
// then says nothing releases it after this long.
const DefaultTimeout = 15
// And the most one turn of the keep-alive loop may take altogether — waiting
// for a request and serving it. A timeout alone cannot bound a client that
// dribbles a byte at a time, because no single wait ever runs out.
const RequestTimeout = 30

const MaxParams = 8

struct Param {
    Name, Value string
}

struct Request {
    // Target is the request line unchanged; Path and RawQuery are its two
    // halves, split at `?`.
    Method, Target, Path, RawQuery, Proto string
    // Who sent it, as dotted quad. A log or a rate limiter wants this and had
    // no way to ask; it is read once per connection rather than per request.
    RemoteAddr string
    Headers    Headers
    Body       []u8
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
    // Set by the server. A response that has sent its head writes through the
    // connection as the handler goes, rather than being collected and measured
    // at the end; `chunked` says whether each write is framed as a chunk or goes
    // out as it is, against a length the client already has.
    conn    ?net.Stream
    sent    bool
    chunked bool
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
func newRequest(buf []u8, from string) Request {
    return Request{Method: "", Target: "", Path: "", RawQuery: "", Proto: "",
                   RemoteAddr: from, Headers: NewHeaders(), Body: buf[0..0],
                   params: [MaxParams]Param{}, pcount: 0}
}

func (mut r *Response) SetHeader(name string, value string) !void {
    try r.Headers.Set(name, value)
}

// Sends the head now and switches to chunked, for a response whose length is
// not known in advance — a long list, a file being read, an event stream. After
// this every write goes out as its own chunk instead of being collected.
//
//     try res.SetHeader("Content-Type", "text/event-stream")
//     try res.Stream(200)
//     for update := feed.Recv() {
//         try res.Printf("data: {}\n\n", update)
//     }
func (mut r *Response) Stream(status u64) !void {
    if r.sent {
        return
    }
    try r.sendHead(status, "Transfer-Encoding: chunked\r\n")
    r.chunked = true
}

// Sends the head now for a body whose length is already known — a file, a blob
// out of a database — and then writes straight through to the connection. The
// client learns how much is coming, which chunked framing cannot tell it, and
// nothing has to be held in memory to be measured.
//
// Write exactly `length` bytes after this. Fewer leaves the client waiting for
// the rest; more is a response that says one thing and does another.
func (mut r *Response) Send(status u64, length u64) !void {
    if r.sent {
        return
    }
    mut room := [32]u8{}
    mut at u64 = 0
    for c in "Content-Length: " {
        room[at] = c
        at += 1
    }
    at += writeDecimal(room[at..room.len], length)
    room[at] = 13
    room[at+1] = 10
    try r.sendHead(status, string(room[0..at+2]))
}

// The status line, one header the caller has already formatted, and whatever
// else the handler set. Leaves the body buffer empty and ready to be written
// through.
func (mut r *Response) sendHead(status u64, framing string) !void {
    c := r.conn orelse return error.NotStreamable
    r.Status = status
    r.sent = true

    mut head := r.body
    head.Reset()
    try head.WriteString("HTTP/1.1 ")
    try head.WriteU64(status)
    try head.WriteByte(32)
    try head.WriteString(StatusText(status))
    try head.WriteString("\r\n")
    try head.WriteString(framing)
    try writeHeaders(&head, &r.Headers)
    try head.WriteString("\r\n")
    try c.Write(head.Bytes())
    head.Reset()
    r.body = head
}

// Digits of a number into the front of a buffer, returning how many. Backwards
// then turned round, which is how this goes without an allocator.
func writeDecimal(mut into []u8, value u64) u64 {
    mut digits := [24]u8{}
    mut count u64 = 0
    mut left := value
    if left == 0 {
        digits[0] = 48
        count = 1
    }
    for left > 0 {
        digits[count] = 48 + u8(left % 10)
        left = left / 10
        count += 1
    }
    for i in 0..count {
        into[i] = digits[count - 1 - i]
    }
    return count
}

// `<length in hex>\r\n`, built backwards into the caller's space and then
// turned round, which is how a number becomes digits with no allocator.
func chunkHead(size u64, mut into []u8) u64 {
    mut digits := [16]u8{}
    mut count u64 = 0
    mut left := size
    if left == 0 {
        digits[0] = 48
        count = 1
    }
    for left > 0 {
        d := u8(left % 16)
        if d < 10 {
            digits[count] = 48 + d
        } else {
            digits[count] = 87 + d
        }
        left = left / 16
        count += 1
    }
    for i in 0..count {
        into[i] = digits[count - 1 - i]
    }
    into[count] = 13
    into[count+1] = 10
    return count + 2
}

// Sends whatever has been written so far. A response that has sent its head is
// otherwise built exactly like a buffered one; the difference is only that the
// buffer is emptied onto the wire instead of measured at the end.
func (mut r *Response) flush() !void {
    if !r.sent || r.body.Len() == 0 {
        return
    }
    c := r.conn orelse return error.NotStreamable
    if !r.chunked {
        try c.Write(r.body.Bytes())
        r.body.Reset()
        return
    }
    mut head := [24]u8{}
    n := chunkHead(r.body.Len(), head[..])
    try c.Write(head[0..n])
    try c.Write(r.body.Bytes())
    try c.WriteString("\r\n")
    r.body.Reset()
}

func (mut r *Response) Write(p []u8) !void {
    // Once the head has gone the buffer has nothing left to do — it is there to
    // measure a body, and the length is already decided. Writing through it
    // instead would cap a response at the size of the buffer, which is how a
    // file larger than one arrived truncated to nothing.
    if r.sent {
        try r.flush()
        try r.writeOut(p)
        return
    }
    try r.body.Write(p)
}

func (mut r *Response) WriteString(s string) !void {
    if r.sent {
        try r.flush()
        try r.writeOut([]u8(s))
        return
    }
    try r.body.WriteString(s)
}

// One write to the connection, framed as a chunk if that is what the head
// promised.
func (mut r *Response) writeOut(p []u8) !void {
    if p.len == 0 {
        return
    }
    c := r.conn orelse return error.NotStreamable
    if !r.chunked {
        try c.Write(p)
        return
    }
    mut head := [24]u8{}
    n := chunkHead(p.len, head[..])
    try c.Write(head[0..n])
    try c.Write(p)
    try c.WriteString("\r\n")
}

// Formats straight into the body; `{}` takes the next argument.
func (mut r *Response) Printf(format string, args ...any) !void {
    try fmt.Format(&r.body, format, args)
    try r.flush()
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
func readRequest(c net.Stream, mut buf []u8, mut req *Request) !u64 {
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

    if isChunked(&req.Headers) {
        got := try readChunked(c, buf, head, have)
        req.Body = buf[head..head+got.body]
        return got.used
    }

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

// What a decoded chunked body came to, and how much of the buffer it took. Two
// numbers rather than a pointer parameter: the framing is interleaved with the
// data, so the caller cannot work the second one out from the first.
struct chunked {
    body u64
    used u64
}

// Decodes a chunked body in place. A chunk's bytes always sit further along than
// where they end up, because the framing before them is dropped, so this can
// compact as it goes without a second buffer.
func readChunked(c net.Stream, mut buf []u8, head u64,
                 already u64) !chunked {
    mut have := already
    mut out := head // where decoded bytes land
    mut at := head  // where the next size line starts

    for {
        mut stop := lineEnd(string(buf[0..have]), at)
        for stop == 0 {
            if have >= buf.len {
                return error.HeadTooLarge
            }
            n := try c.Read(buf[have..buf.len])
            if n == 0 {
                return error.Truncated
            }
            have += n
            stop = lineEnd(string(buf[0..have]), at)
        }

        size := try chunkSize(string(buf[at..stop]))
        at = stop + 2

        // The last chunk is empty; what follows is trailers nobody reads and
        // the blank line, neither of which the body needs.
        if size == 0 {
            return chunked{body: out - head, used: have}
        }
        if out + size > buf.len {
            return error.BodyTooLarge
        }

        for have < at + size + 2 {
            if have >= buf.len {
                return error.BodyTooLarge
            }
            n := try c.Read(buf[have..buf.len])
            if n == 0 {
                return error.Truncated
            }
            have += n
        }
        for i in 0..size {
            buf[out + i] = buf[at + i]
        }
        out += size
        at += size + 2
    }
    return chunked{body: out - head, used: have}
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

func writeResponse(c net.Stream, mut res *Response, keep bool,
                   mut out *bytes.Buffer) !void {
    // A response that sent its own head has nothing left but what is still in
    // the buffer — and, if it was chunked, the chunk that says there are no more.
    if res.sent {
        try res.flush()
        if res.chunked {
            try c.WriteString("0\r\n\r\n")
        }
        return
    }
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

func handleConn(c net.Stream, h Handler, mut a mem.Allocator,
                s *Server, from string) !void {
    mut buf := mem.Alloc[u8](a, MaxHead) orelse return error.OutOfMemory
    mut out := try bytes.New(a, 1024)
    mut body := try bytes.New(a, 1024)

    for {
        // One deadline per turn, covering the wait for a request and the
        // serving of it. An idle connection is dropped when it runs out, which
        // is what a keep-alive idle timeout is.
        c.SetDeadline(time.Now().Add(time.Seconds(RequestTimeout))) catch {}

        mut req := newRequest(buf, from)
        got := try readRequest(c, buf, &req)
        if got == 0 {
            return
        }

        body.Reset()
        mut res := Response{Status: 200, Headers: NewHeaders(), body: body,
                            conn: c, sent: false, chunked: false}
        h.Serve(&req, &res) catch {
            res.Status = 500
            s.failed.Add(1)
        }
        s.served.Add(1)
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
    // Counted rather than guessed: a server nobody can see inside is a server
    // nobody can run. Atomics because every connection is its own task and they
    // all count themselves.
    live     atomic[u64]
    accepted atomic[u64]
    served   atomic[u64]
    failed   atomic[u64]
}

// Loopback only, which is what a test wants and not what a server does.
func Listen(port i32) !Server {
    return Server{listener: try net.Listen(port), live: atomic[u64](0),
                  accepted: atomic[u64](0), served: atomic[u64](0),
                  failed: atomic[u64](0)}
}

// The address to bind, empty meaning every interface. `share` lets another
// process hold the same port, which is how one is replaced by another without
// dropping connections in between.
func ListenOn(host string, port i32, share bool) !Server {
    return Server{listener: try net.ListenOn(host, port, share),
                  live: atomic[u64](0), accepted: atomic[u64](0),
                  served: atomic[u64](0), failed: atomic[u64](0)}
}

// Connections being served right now.
func (s *Server) Live() u64 {
    return s.live.Load()
}

// Connections taken since the server started.
func (s *Server) Accepted() u64 {
    return s.accepted.Load()
}

// Requests answered, and of those the ones whose handler failed and became a
// 500. A rate worth watching.
func (s *Server) Served() u64 {
    return s.served.Load()
}

func (s *Server) Failed() u64 {
    return s.failed.Load()
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

    // Asked once, on the task's own stack: the address does not change and the
    // request only borrows it.
    mut room := [24]u8{}
    peer := conn.Peer(room[..]) catch net.Peer{Address: "", Port: 0}
    from := peer.Address

    handleConn(&conn, h, &arena, s, from) catch {}
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
            s.accepted.Add(1)
            spawn serveConn(c, h, s)
        }
    }
}
