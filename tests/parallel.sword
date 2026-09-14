// expect: 42
// The body captures xs from the enclosing frame; the reduction gives each
// worker a private accumulator and folds them once at the end.

import "std/mem"

func main() !int {
    mut backing := [16384]u8{}
    mut arena := mem.NewArena(backing[..])

    n := 1000
    mut xs := mem.Alloc[i64](&arena, u64(n)) orelse return error.OutOfMemory
    for i in 0..xs.len {
        xs[i] = i64(i)
    }

    parallel for i in 0..n {
        xs[i] = xs[i] * 2
    }

    mut total i64 = 0
    parallel for i in 0..n reduce(+: total) {
        total += xs[i]
    }

    if total != 999000 {
        return 1
    }
    return 42
}
