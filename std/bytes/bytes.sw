package bytes

import "std/mem"

// A byte buffer that grows. It keeps the allocator it was built with, so
// appending never has to be told where memory comes from twice.
struct Buffer {
    data  []u8
    count u64
    a     mem.Allocator
}

const MinCapacity = 64

func New(mut a mem.Allocator, capacity u64) !Buffer {
    mut want := capacity
    if want < MinCapacity {
        want = MinCapacity
    }
    room := mem.Alloc[u8](a, want) orelse return error.OutOfMemory
    return Buffer{data: room, count: 0, a: a}
}

func (b *Buffer) Len() u64 {
    return b.count
}

func (b *Buffer) Bytes() []u8 {
    return b.data[0..b.count]
}

func (b *Buffer) Str() string {
    return string(b.data[0..b.count])
}

func (mut b *Buffer) Reset() {
    b.count = 0
}

func (mut b *Buffer) Free() {
    mem.Free(b.a, b.data)
    b.count = 0
}

// Doubling keeps appending amortised: a run of small writes copies each byte
// a constant number of times overall.
func (mut b *Buffer) reserve(extra u64) !void {
    needed := b.count + extra
    if needed <= b.data.len {
        return
    }
    mut want := b.data.len * 2
    for want < needed {
        want *= 2
    }
    mut room := mem.Alloc[u8](b.a, want) orelse return error.OutOfMemory
    for i in 0..b.count {
        room[i] = b.data[i]
    }
    mem.Free(b.a, b.data)
    b.data = room
}

func (mut b *Buffer) WriteByte(c u8) !void {
    try b.reserve(1)
    b.data[b.count] = c
    b.count += 1
}

func (mut b *Buffer) Write(from []u8) !void {
    try b.reserve(from.len)
    for i in 0..from.len {
        b.data[b.count + i] = from[i]
    }
    b.count += from.len
}

func (mut b *Buffer) WriteString(s string) !void {
    try b.Write([]u8(s))
}

// Decimal, no padding. Writing the digits backwards into a scratch array and
// reversing avoids needing to know the length up front.
func (mut b *Buffer) WriteU64(v u64) !void {
    if v == 0 {
        try b.WriteByte(48)
        return
    }
    mut digits := [20]u8{}
    mut n := v
    mut i u64 = 0
    for n > 0 {
        digits[i] = u8(48 + n % 10)
        n /= 10
        i += 1
    }
    for k in 0..i {
        try b.WriteByte(digits[i - 1 - k])
    }
}
