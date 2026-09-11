// expect: 17
// expect-output: using packages

import "std/io"
import "std/mem"

func main() !int {
    try io.Print("using packages\n")

    mut backing := [128]u8{}
    mut arena := mem.NewArena(backing[..])
    mut sys := mem.NewSystem()

    mut p := arena.Alloc(10, 8) orelse return error.OutOfMemory
    p[0] = 7

    mut q := sys.Alloc(20, 8) orelse return error.OutOfMemory
    q[0] = 5
    sys.Release(q, 20)

    // 7 from the arena block, 10 bytes used, nothing still live.
    return int(p[0]) + int(arena.Used()) + int(sys.Live())
}
