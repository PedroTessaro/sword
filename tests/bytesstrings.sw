// expect: 42
// Buffer growth, number formatting, and the byte-level string helpers a
// protocol parser needs.

import "std/mem"
import "std/bytes"
import "std/strings"

func main() !int {
    mut backing := [8192]u8{}
    mut arena := mem.NewArena(backing[..])

    mut b := try bytes.New(&arena, 8)
    try b.WriteString("count=")
    try b.WriteU64(1234)
    try b.WriteByte(33)
    if !strings.Equal(b.Str(), "count=1234!") {
        return 1
    }

    if !strings.HasPrefix("GET /health HTTP/1.1", "GET ") {
        return 2
    }
    if strings.IndexByte("a:b", 58) != 1 {
        return 3
    }
    if strings.Index("hello world", "wor") != 6 {
        return 4
    }
    if !strings.EqualFold("Content-Length", "content-length") {
        return 5
    }
    if !strings.Equal(strings.TrimSpace("  hi \r\n"), "hi") {
        return 6
    }
    n := try strings.ParseU64("4096")
    if n != 4096 {
        return 7
    }
    return 42
}
