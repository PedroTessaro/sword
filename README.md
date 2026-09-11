# Sword

Sword is a compiled systems language with Go's syntax and Zig's memory model.
It has no garbage collector, nothing allocates behind your back, and the
compiler refuses to build a program where two tasks can touch the same memory
at the same time.

```sword
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
        for off, part in dst.chunks(8) {
            spawn square(src[off..off+part.len], part)
        }
    }

    return int(dst[63] % 100)
}
```

Eight tasks write into eight pieces of the same array. That compiles because
`chunks` hands out pieces that cannot overlap. Slice `dst` by hand inside the
loop instead and the compiler stops you, because then it has no way to know the
pieces are distinct.

## Install

```sh
git clone https://github.com/PedroTessaro/sword
cd sword
make
make install
```

```sword
import "std/io"

func main() !int {
    try io.Printf("{} workers, {.1}ms each\n", 8, 12.5)
    return 0
}
```

That puts `shield` (the compiler) and `swordls` (the language server) in
`~/.local/bin`. See [docs/install.md](docs/install.md) for prefixes, editor
setup and what the build needs.

## Learn it

- **[A tour of Sword](docs/tour.md)** — start here. Every section is a program
  you can run, and they build on each other from hello world to parallel code.
- **[Concurrency](docs/concurrency.md)** — how tasks, partitions and the race
  checker fit together, and why they are shaped the way they are.
- **[HTTP](docs/http.md)** — writing a server and a client with `std/http`.
- **[Reference](docs/reference.md)** — types, operators, keywords, standard
  library. For looking things up once you know the language.

## What it looks like

Three commitments run through every design decision:

**Nothing allocates implicitly.** A function that touches the heap takes an
`Allocator` parameter. There is no other way, including in the standard
library, so a signature without one cannot allocate.

**A task's lifetime is a lexical scope.** `spawn` only exists inside `scope`,
and the closing brace is the join. That is what makes it safe to lend a task a
slice of memory the parent owns.

**Immutable by default.** `x := 1` cannot be reassigned; `mut x := 1` can. That
is not a style preference — immutable data crosses between tasks with no
synchronisation at all, which is what the race checker leans on.

## Status

Sword compiles to native code through LLVM and the language is usable, but it
is young. Working today: functions with variadic parameters, methods, interfaces with
dynamic dispatch, generics over both functions and structs, floating point,
top-level constants, atomics, structs,
slices, strings, pointers, optionals, error unions, `defer`, packages, and the
whole concurrency story with a work-stealing scheduler behind it.

The standard library covers memory (`std/mem`), byte buffers (`std/bytes`),
byte-level strings (`std/strings`), generic containers (`std/collections`),
printing and formatting (`std/io`, `std/fmt`), JSON (`std/json`), TCP
(`std/net`), and HTTP/1.1 with keep-alive, server and client (`std/http`).

Not there yet: `shared[T]`, channels, and function values.

## License

MIT.
