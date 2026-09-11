// expect: 42
// A partition is the only way to hand pieces of one slice to different tasks.
// The compiler knows the pieces cannot overlap, so it does not have to prove
// anything about the indices.

import "std/mem"

func square(xs []i64, mut out []i64) {
    for i in 0..out.len {
        out[i] = xs[i] * xs[i]
    }
}

func main() !int {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])

    mut src := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    mut dst := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    for i in 0..src.len {
        src[i] = i64(i)
    }

    scope {
        // Every task writes its own piece of dst; they all read src, which is
        // fine because nobody writes it.
        for off, part in dst.chunks(8) {
            spawn square(src[off..off+part.len], part)
        }
    }

    mut total i64 = 0
    for i in 0..dst.len {
        total += dst[i]
    }
    if total != 85344 {
        return 1
    }
    return 42
}
