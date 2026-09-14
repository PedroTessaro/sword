// expect: 0
// expect-output: GET:/health took 1.50ms

import "std/fmt"
import "std/io"
import "std/mem"
import "std/strings"

func main() !int {
    mut room := [8192]u8{}
    mut arena := mem.NewArena(room[..])

    s := try fmt.Sprintf(&arena, "{}:{} took {.2}ms", "GET", "/health", 1.5)
    if !strings.Equal(s, "GET:/health took 1.50ms") {
        return 1
    }
    try io.Printf("{}\n", s)
    return 0
}
