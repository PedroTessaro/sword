// expect: 42
// expect-output: list works
// A generic vector: growth by doubling, and a separate compiled copy per
// element type.

import "std/collections"
import "std/io"
import "std/mem"

func main() !int {
    mut room := [16384]u8{}
    mut arena := mem.NewArena(room[..])

    mut xs := try collections.NewList[i64](&arena, 2)
    for i in 0..100 {
        try xs.Push(i64(i) * 2)
    }
    if xs.Len() != 100 || xs.At(50) != 100 {
        return 1
    }
    if xs.Cap() < 100 {
        return 2
    }

    mut total i64 = 0
    view := xs.Slice()
    for i in 0..view.len {
        total += view[i]
    }
    if total != 9900 {
        return 3
    }

    last := xs.Pop() orelse return 4
    if last != 198 || xs.Len() != 99 {
        return 5
    }

    mut names := try collections.NewList[bool](&arena, 4)
    try names.Push(true)
    if !names.At(0) {
        return 6
    }
    try io.Print("list works\n")
    return 42
}
