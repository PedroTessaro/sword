// expect: 41
// mem.Alloc[T] is generic in the library and instantiated here, once per
// element type.

import "std/mem"

struct Point {
    x i32
    y i32
}

func main() !int {
    mut backing := [256]u8{}
    mut arena := mem.NewArena(backing[..])

    mut xs := mem.Alloc[i64](&arena, 4) orelse return error.OutOfMemory
    for i in 0..xs.len {
        xs[i] = i64(i) * 10
    }

    mut ps := mem.Alloc[Point](&arena, 2) orelse return error.OutOfMemory
    ps[0] = Point{x: 3, y: 4}
    ps[1] = Point{x: 5, y: 6}

    mem.Free(&arena, xs)
    return int(xs[3]) + int(ps[1].x) + int(ps[1].y)
}
