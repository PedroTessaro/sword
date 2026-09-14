// expect: 42
// Many tasks may read the same memory; only writing needs a partition.

import "std/mem"

func total(xs []i64, mut out []i64) {
    mut sum i64 = 0
    for i in 0..xs.len {
        sum += xs[i]
    }
    out[0] = sum
}

func main() !int {
    mut backing := [2048]u8{}
    mut arena := mem.NewArena(backing[..])

    mut src := mem.Alloc[i64](&arena, 16) orelse return error.OutOfMemory
    mut sums := mem.Alloc[i64](&arena, 4) orelse return error.OutOfMemory
    for i in 0..src.len {
        src[i] = i64(i)
    }

    scope {
        for slot in sums.chunks(1) {
            spawn total(src, slot)
        }
    }

    mut all i64 = 0
    for i in 0..sums.len {
        all += sums[i]
    }
    if all != 480 {
        return 1
    }
    return 42
}
