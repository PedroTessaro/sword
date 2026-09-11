package http

import "std/bytes"
import "std/mem"
import "std/net"
import "std/strings"

// The body points into memory taken from the allocator that was passed in, so
// it stays valid until that allocator is reset or freed.
struct ClientResponse {
    Status  u64
    Headers Headers
    Body    []u8
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

func request(host string, port i32, method string, path string,
             contentType string, body []u8,
             mut a mem.Allocator) !ClientResponse {
    mut req := try bytes.New(a, 512)
    try req.WriteString(method)
    try req.WriteByte(32)
    try req.WriteString(path)
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

    mut c := try net.Dial(host, port)
    c.Write(req.Bytes()) catch |e| {
        c.Close()
        return e
    }

    mut buf := mem.Alloc[u8](a, MaxHead) orelse return error.OutOfMemory
    res := readResponse(&c, buf) catch |e| {
        c.Close()
        return e
    }
    c.Close()
    return res
}

func Get(host string, port i32, path string,
         mut a mem.Allocator) !ClientResponse {
    mut empty := [0]u8{}
    return try request(host, port, "GET", path, "", empty[..], a)
}

func Post(host string, port i32, path string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse {
    return try request(host, port, "POST", path, contentType, body, a)
}
