package collections

import "std/mem"

// A vector that grows. It keeps the allocator it was built with, so pushing
// never has to be told where memory comes from twice.
struct List[T] {
    items []T
    count u64
    a     mem.Allocator
}

const MinCapacity = 8

func NewList[T](mut a mem.Allocator, capacity u64) !List[T] {
    mut want := capacity
    if want < MinCapacity {
        want = MinCapacity
    }
    room := mem.Alloc[T](a, want) orelse return error.OutOfMemory
    return List[T]{items: room, count: 0, a: a}
}

func (l *List[T]) Len() u64 {
    return l.count
}

func (l *List[T]) Cap() u64 {
    return l.items.len
}

// A window onto the live elements. It stops being valid the next time the list
// grows, so do not keep it across a Push.
func (l *List[T]) Slice() []T {
    return l.items[0..l.count]
}

func (l *List[T]) At(i u64) T {
    return l.items[i]
}

func (mut l *List[T]) Set(i u64, v T) {
    l.items[i] = v
}

func (mut l *List[T]) Reset() {
    l.count = 0
}

func (mut l *List[T]) Free() {
    mem.Free(l.a, l.items)
    l.count = 0
}

// Doubling keeps pushing amortised: a run of pushes copies each element a
// constant number of times overall.
func (mut l *List[T]) Push(v T) !void {
    if l.count >= l.items.len {
        mut room := mem.Alloc[T](l.a, l.items.len * 2) orelse
            return error.OutOfMemory
        for i in 0..l.count {
            room[i] = l.items[i]
        }
        mem.Free(l.a, l.items)
        l.items = room
    }
    l.items[l.count] = v
    l.count += 1
}

func (mut l *List[T]) Pop() ?T {
    if l.count == 0 {
        return nil
    }
    l.count -= 1
    return l.items[l.count]
}
