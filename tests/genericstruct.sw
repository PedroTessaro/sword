// expect: 42
// One template, two instantiations, each with its own copy of the methods.

import "std/mem"

error Full = "there is no room left in it"

struct List[T] {
    items []T
    count u64
}

func (l *List[T]) Len() u64 {
    return l.count
}

func (mut l *List[T]) Push(v T) !void {
    if l.count >= l.items.len {
        return error.Full
    }
    l.items[l.count] = v
    l.count += 1
}

func (l *List[T]) At(i u64) T {
    return l.items[i]
}

func main() !int {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])

    mut nums := mem.Alloc[i64](&arena, 8) orelse return error.OutOfMemory
    mut a := List[i64]{items: nums, count: 0}
    try a.Push(10)
    try a.Push(32)

    mut flags := mem.Alloc[bool](&arena, 4) orelse return error.OutOfMemory
    mut b := List[bool]{items: flags, count: 0}
    try b.Push(true)

    if b.Len() != 1 {
        return 1
    }
    return int(a.At(0) + a.At(1)) * int(b.Len())
}
