// expect: 42
// A constraint is just an interface, but the call is resolved at
// instantiation: no vtable, no indirection.

interface Sized {
    Size() u64
}

struct Box {
    w u64
    h u64
}

func (b *Box) Size() u64 {
    return b.w * b.h
}

func Total[T: Sized](a *T, b *T) u64 {
    return a.Size() + b.Size()
}

func main() int {
    mut p := Box{w: 3, h: 4}
    mut q := Box{w: 5, h: 6}
    return int(Total(&p, &q))
}
