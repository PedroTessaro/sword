package mem

extern func malloc(n u64) ?[*]u8
extern func free(p [*]u8)

// Everything that allocates takes one of these, so a signature without it
// cannot touch the heap.
interface Allocator {
    Alloc(n u64, align u64) ?[*]u8
    Release(p [*]u8, n u64)
}

func Align(offset u64, align u64) u64 {
    return (offset + align - 1) / align * align
}

// Hands every request to the C allocator and keeps track of what is still out.
struct System {
    live u64
}

func NewSystem() System {
    return System{live: 0}
}

func (mut s *System) Alloc(n u64, align u64) ?[*]u8 {
    p := malloc(n) orelse return nil
    s.live += n
    return p
}

func (mut s *System) Release(p [*]u8, n u64) {
    free(p)
    s.live -= n
}

func (s *System) Live() u64 {
    return s.live
}

// Bumps a pointer through a buffer it does not own. Individual releases do
// nothing; the whole arena is reset at once, which is the point.
struct Arena {
    buf    [*]u8
    cap    u64
    offset u64
}

func NewArena(backing []u8) Arena {
    return Arena{buf: [*]u8(&backing[0]), cap: backing.len, offset: 0}
}

func (mut a *Arena) Alloc(n u64, align u64) ?[*]u8 {
    start := Align(a.offset, align)
    if start + n > a.cap {
        return nil
    }
    a.offset = start + n
    return a.buf + start
}

func (mut a *Arena) Release(p [*]u8, n u64) {}

func (mut a *Arena) Reset() {
    a.offset = 0
}

func (a *Arena) Used() u64 {
    return a.offset
}

// Typed allocation. This is the whole reason generics come before the
// runtime: without them an allocator can only hand out bytes.
func Alloc[T](mut a Allocator, n u64) ?[]T {
    bytes := a.Alloc(n * sizeof[T](), alignof[T]()) orelse return nil
    p := [*]T(bytes)
    return p[0..n]
}

func Free[T](mut a Allocator, xs []T) {
    a.Release([*]u8(xs.ptr), xs.len * sizeof[T]())
}
