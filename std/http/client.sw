package http

import "std/bytes"
import "std/mem"
import "std/net"
import "std/strings"
import "std/time"

// A request that hangs is worse than one that fails, so the package-level
// helpers come with a limit rather than none.
const DefaultConnectTimeout = 10
const DefaultReadTimeout = 30

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
}

func NewClient() Client {
    return Client{Connect: time.Seconds(DefaultConnectTimeout),
                  Read: time.Seconds(DefaultReadTimeout)}
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

func readResponse(mut c *net.Conn, mut buf []u8) !ClientResponse {
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

    want := try bodyLength(&out.Headers)
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

func (c *Client) Do(host string, port i32, method string, target string,
                    contentType string, body []u8,
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

    mut conn := try net.DialTimeout(host, port, c.Connect)
    conn.SetTimeout(c.Read) catch {}
    conn.Write(req.Bytes()) catch |e| {
        conn.Close()
        return e
    }

    mut buf := mem.Alloc[u8](a, MaxHead) orelse return error.OutOfMemory
    res := readResponse(&conn, buf) catch |e| {
        conn.Close()
        return e
    }
    conn.Close()
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
