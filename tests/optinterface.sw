// expect: 42
// expect-output: optional interfaces work
// A `?Interface` taking a pointer to a struct used to be rejected outright: the
// conversion that builds the { data, vtable } pair only looked at a bare
// interface, never at one inside an optional. Which is what a connection that may
// or may not be there is.

import "std/io"

interface Sink {
    Put(b u8) !void
    Total() u64
}

struct Counter {
    seen atomic[u64]
}

func (c *Counter) Put(b u8) !void {
    c.seen.Add(u64(b))
    return
}

func (c *Counter) Total() u64 {
    return c.seen.Load()
}

struct Pipe {
    out ?Sink
}

// Through a parameter, through a field, and through nothing at all.
func drain(into ?Sink, times u64) !u64 {
    s := into orelse return 0
    for i in 0..times {
        try s.Put(2)
    }
    return s.Total()
}

func main() !int {
    mut c := Counter{seen: atomic[u64](0)}

    // A parameter.
    if (try drain(&c, 3)) != 6 {
        return 1
    }

    // A field set in a literal.
    mut p := Pipe{out: &c}
    inner := p.out orelse return 2
    try inner.Put(4)
    if inner.Total() != 10 {
        return 3
    }

    // A field assigned later, and one left empty.
    mut empty := Pipe{out: nil}
    if empty.out != nil {
        return 4
    }
    empty.out = &c
    second := empty.out orelse return 5
    try second.Put(1)
    if second.Total() != 11 {
        return 6
    }

    // And nothing there is not an error, it is nothing.
    if (try drain(nil, 9)) != 0 {
        return 7
    }

    try io.Print("optional interfaces work\n")
    return 42
}
