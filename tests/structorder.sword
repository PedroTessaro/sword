// expect: 42
// expect-output: layout does not depend on order
// A struct's size depends on the structs it holds by value, and the order they
// are declared in says nothing about that. Laying them out in declaration order
// gave `Outer` a size with `Inner` counted as nothing — silently, and the only
// symptom was a copy that moved too few bytes, so the last field of the struct
// came back as whatever had been on the stack.

import "std/io"

// Declared before the struct it contains, which is the whole point.
struct Outer {
    inner Inner
    flag  bool
}

struct Inner {
    room [32]u64
    n    u64
}

// Deeper, and through a wrapper: `?T` and `!T` are built while a field is being
// resolved, before the T they wrap has a size at all.
struct Holder {
    one  ?Outer
    two  Middle
    mark u64
}

struct Middle {
    outer Outer
    tag   u64
}

func build() !Outer {
    mut i := Inner{room: [32]u64{}, n: 7}
    i.room[31] = 99
    return Outer{inner: i, flag: true}
}

func hold() !Holder {
    o := try build()
    return Holder{one: o, two: Middle{outer: o, tag: 5}, mark: 12345}
}

func main() !int {
    // The sizes have to add up, whatever order the declarations came in.
    if sizeof[Outer]() < sizeof[Inner]() {
        return 1
    }
    if sizeof[Middle]() < sizeof[Outer]() + 8 {
        return 2
    }
    if sizeof[Holder]() < sizeof[Middle]() + sizeof[Outer]() {
        return 3
    }

    // And a value has to survive being copied out of a function.
    o := try build()
    if !o.flag || o.inner.n != 7 || o.inner.room[31] != 99 {
        return 4
    }

    h := try hold()
    if h.mark != 12345 || h.two.tag != 5 {
        return 5
    }
    if !h.two.outer.flag || h.two.outer.inner.room[31] != 99 {
        return 6
    }
    inside := h.one orelse return 7
    if !inside.flag || inside.inner.n != 7 {
        return 8
    }

    try io.Printf("layout does not depend on order: {} {} {}\n",
                  sizeof[Outer](), sizeof[Middle](), sizeof[Holder]())
    return 42
}
