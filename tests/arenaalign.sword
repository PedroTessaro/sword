// expect: 42
// expect-output: arena memory is aligned
// An arena used to align the offset into its buffer rather than the address it
// hands out. A buffer of bytes may begin anywhere, so everything it gave back was
// off by however much the buffer was — which ordinary loads on arm64 forgive and
// atomics do not. The first thing to notice was a channel, whose mutex lives in
// arena memory and faulted on itself.

import "std/io"
import "std/mem"

struct Holder {
    gate  shared[u64]
    count atomic[u64]
}

func main() !int {
    // Deliberately odd: one byte of padding in front of the buffer, which is
    // exactly what a real program does by accident.
    mut room := [8192]u8{}
    mut arena := mem.NewArena(room[1..room.len])

    // Every allocation lands on its type's alignment, measured rather than
    // assumed.
    mut ones := mem.Alloc[u64](&arena, 4) orelse return 1
    if u64(ones.ptr) % 8 != 0 {
        return 2
    }
    mut holders := mem.Alloc[Holder](&arena, 2) orelse return 3
    if u64(holders.ptr) % alignof[Holder]() != 0 {
        return 4
    }

    // And the mutex in there works, which is the whole point: locking it is an
    // atomic operation on an address that has to be aligned.
    holders[0] = Holder{gate: shared[u64](7), count: atomic[u64](0)}
    lock v := &holders[0].gate {
        *v += 1
    }
    holders[0].count.Add(2)

    mut seen u64 = 0
    lock v := &holders[0].gate {
        seen = *v
    }
    if seen != 8 || holders[0].count.Load() != 2 {
        return 5
    }

    // A byte buffer that starts aligned must behave the same way.
    mut straight := mem.NewArena(room[..])
    mut more := mem.Alloc[u64](&straight, 1) orelse return 6
    if u64(more.ptr) % 8 != 0 {
        return 7
    }

    try io.Print("arena memory is aligned\n")
    return 42
}
