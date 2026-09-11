// expect: 42
// expect-output: reductions work
// The body combines one element into the worker's private copy; the reduce
// clause says how the copies are folded together at the end.

import "std/io"
import "std/mem"

func main() !int {
    mut backing := [16384]u8{}
    mut arena := mem.NewArena(backing[..])

    n := 512
    mut xs := mem.Alloc[i64](&arena, u64(n)) orelse return error.OutOfMemory
    for i in 0..xs.len {
        xs[i] = i64(i % 37) - 8
    }

    mut total i64 = 0
    parallel for i in 0..n reduce(+: total) {
        total += xs[i]
    }

    mut biggest i64 = 0
    parallel for i in 0..n reduce(max: biggest) {
        if xs[i] > biggest {
            biggest = xs[i]
        }
    }

    mut smallest i64 = 0
    parallel for i in 0..n reduce(min: smallest) {
        if xs[i] < smallest {
            smallest = xs[i]
        }
    }

    mut bits u64 = 0
    parallel for i in 0..n reduce(|: bits) {
        bits |= u64(i % 8)
    }

    mut sum f64 = 0.0
    parallel for i in 0..n reduce(+: sum) {
        sum += 0.5
    }

    mut expected i64 = 0
    for i in 0..xs.len {
        expected += xs[i]
    }
    if total != expected { return 1 }
    if biggest != 28 { return 2 }
    if smallest != -8 { return 3 }
    if bits != 7 { return 4 }
    if sum != 256.0 { return 5 }
    try io.Print("reductions work\n")
    return 42
}
