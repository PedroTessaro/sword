package http

import "std/bytes"
import "std/strings"

const MaxHeaders = 32
const MaxHead = 16384

struct Header {
    Name, Value string
}

// Both a request and a response carry the same header block, so the parsing
// and lookup live in one place.
struct Headers {
    items [MaxHeaders]Header
    count u64
}

func (h *Headers) Len() u64 {
    return h.count
}

func (h *Headers) At(i u64) Header {
    return h.items[i]
}

// Header names are case-insensitive, which is the whole reason this is not a
// plain comparison.
func (h *Headers) Get(name string) string {
    for i in 0..h.count {
        if strings.EqualFold(h.items[i].Name, name) {
            return h.items[i].Value
        }
    }
    return ""
}

func (h *Headers) Has(name string) bool {
    for i in 0..h.count {
        if strings.EqualFold(h.items[i].Name, name) {
            return true
        }
    }
    return false
}

func (mut h *Headers) Set(name string, value string) !void {
    for i in 0..h.count {
        if strings.EqualFold(h.items[i].Name, name) {
            h.items[i].Value = value
            return
        }
    }
    if h.count >= MaxHeaders {
        return error.TooManyHeaders
    }
    h.items[h.count] = Header{Name: name, Value: value}
    h.count += 1
}

func (mut h *Headers) Reset() {
    h.count = 0
}

// Where the blank line that ends the header block finishes, or zero when it
// is not in there yet. Zero is unambiguous because a header block is never
// shorter than the four bytes of the blank line itself — whereas `s.len` would
// collide with the common case of the block ending exactly at the end of what
// has been read so far.
func headerEnd(s string) u64 {
    if s.len < 4 {
        return 0
    }
    for i in 0..s.len - 3 {
        if s[i] == 13 && s[i+1] == 10 && s[i+2] == 13 && s[i+3] == 10 {
            return i + 4
        }
    }
    return 0
}

// Splits off the next CRLF-terminated line. `rest` is what follows.
struct line {
    text string
    rest string
}

func nextLine(s string) line {
    for i in 0..s.len {
        if s[i] == 13 && i + 1 < s.len && s[i+1] == 10 {
            return line{text: s[0..i], rest: s[i+2..s.len]}
        }
    }
    return line{text: s, rest: s[s.len..s.len]}
}

// `Name: Value`, with surrounding space on the value ignored.
func parseHeaders(block string, mut into *Headers) !void {
    mut rest := block
    for rest.len > 0 {
        cut := nextLine(rest)
        rest = cut.rest
        if cut.text.len == 0 {
            return
        }
        colon := strings.IndexByte(cut.text, 58)
        if colon == cut.text.len {
            return error.BadHeader
        }
        name := strings.TrimSpace(cut.text[0..colon])
        value := strings.TrimSpace(cut.text[colon+1..cut.text.len])
        try into.Set(name, value)
    }
}

func bodyLength(h *Headers) !u64 {
    got := h.Get("Content-Length")
    if got.len == 0 {
        return 0
    }
    return try strings.ParseU64(got)
}

func writeHeaders(mut out *bytes.Buffer, h *Headers) !void {
    for entry in h.items[0..h.count] {
        try out.WriteString(entry.Name)
        try out.WriteString(": ")
        try out.WriteString(entry.Value)
        try out.WriteString("\r\n")
    }
}

func StatusText(code u64) string {
    switch code {
    case 200:
        return "OK"
    case 201:
        return "Created"
    case 204:
        return "No Content"
    case 301:
        return "Moved Permanently"
    case 302:
        return "Found"
    case 400:
        return "Bad Request"
    case 401:
        return "Unauthorized"
    case 403:
        return "Forbidden"
    case 404:
        return "Not Found"
    case 405:
        return "Method Not Allowed"
    case 413:
        return "Payload Too Large"
    case 500:
        return "Internal Server Error"
    default:
        return "Unknown"
    }
}
