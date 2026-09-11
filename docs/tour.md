# A tour of Sword

This is meant to be read start to finish, at a terminal. Every program here
runs. If you know Go or C you will recognise most of the shape of things, and
the places where Sword differs are the places worth slowing down for.

## Running a program

```sword
import "std/io"

func main() !int {
    try io.Print("hello\n")
    return 0
}
```

```sh
shield hello.sw -o hello
./hello
```

`main` returns `int`, which becomes the process exit status. The `!` in front
means it can fail; more on that later. If you have nothing to say to the
outside world, `func main() int` is fine on its own.

There are no semicolons. The lexer inserts them at line ends, the same way Go
does, so a statement ends where the line does.

To print something other than a string, `Printf` takes values of any type:

```sword
import "std/io"

func main() !int {
    count := 42
    ratio := 1.5
    try io.Printf("{} items, {} each\n", count, ratio)
    try io.Println("or just this:", count, ratio, true)
    return 0
}
```

`{}` takes the next argument. There are no type letters like `%d`, because
there is nothing for them to do — the argument already knows what it is, so a
format string cannot disagree with it. `{x}` asks for hex and `{.3}` for a
float with three places.

Neither of these needs an allocator. They format into a block of stack and
flush as it fills, which matters because printing is the first thing anybody
writes.

## Values

`:=` binds a name to a value. The binding is **immutable**:

```sword
func main() int {
    x := 21
    return x * 2
}
```

Write `x = 5` after that and the compiler stops you. To get a variable you can
assign to, say so:

```sword
func main() int {
    mut total := 0
    for i in 0..10 {
        total += i
    }
    return total
}
```

This is not a style preference. Immutable data can be shared between tasks with
no locking at all, and the concurrency checker leans on that. Making it the
default means the common case is also the safe one.

Number literals have no type until something gives them one. `21` above became
`int` because nothing else claimed it. When you need a specific width, either
declare the type or convert:

```sword
func main() int {
    mut count i32 = 0      // declared
    count += 1

    big := i64(count)      // converted
    return int(big)
}
```

Sword never narrows a value for you. `i64(x)` is how you say you meant it.

## Functions

```sword
func add(a int, b int) int {
    return a + b
}

func main() int {
    return add(20, 22)
}
```

Types come after names, as in Go. A function with no result just omits it. When
several parameters share a type, write it once:

```sword
func area(w, h u64) u64 {
    return w * h
}
```

A last parameter can gather whatever is left:

```sword
func total(label string, xs ...i64) i64 {
    mut sum i64 = 0
    for i in 0..xs.len {
        sum += xs[i]
    }
    return sum
}

func main() int {
    return int(total("a", 20, 22))
}
```

Inside the function `xs` is an ordinary `[]i64`. Gathering `...any` is how
`Printf` accepts a mixed list; passing an already-gathered list on to another
such function is written `f(xs...)`.

Parameters are immutable too. When a function needs to write through one, the
parameter says so, and the caller has to have that permission to give away:

```sword
func double(mut xs []i64) {
    for i in 0..xs.len {
        xs[i] *= 2
    }
}

func main() int {
    mut values := [4]i64{1, 2, 3, 4}
    mut view := values[..]
    double(view)
    return int(view[3])
}
```

Drop the `mut` from `view` and the call fails to compile: handing a value to a
`mut` parameter is granting write access, and only something you can write can
grant it.

## Control flow

`if` needs no parentheses and always needs braces:

```sword
func classify(n int) int {
    if n < 0 {
        return -1
    } else if n == 0 {
        return 0
    }
    return 1
}

func main() int {
    return classify(7) + classify(-3) + 42
}
```

`for` has three forms and no others:

```sword
func main() int {
    mut sum := 0

    for i in 0..5 {          // a range
        sum += i
    }

    mut n := 0
    for n < 3 {              // a condition
        n += 1
        sum += n
    }

    for {                    // forever, until something leaves
        sum += 1
        if sum > 20 {
            break
        }
    }

    return sum
}
```

There is a fourth form, `for part in xs.chunks(n)`, but it exists for
concurrency and is covered there.

## Arrays and slices

An array has its length in its type and is copied when assigned:

```sword
func main() int {
    mut digits := [4]i32{7, 8, 9, 10}
    copy := digits          // a real copy
    digits[0] = 0
    return int(copy[0])     // still 7
}
```

`[64]u8{}` is the zeroed array. Any other literal has to be written out in
full.

A slice is a pointer and a length — two words that point at somebody else's
elements:

```sword
func sum(xs []i64) i64 {
    mut total i64 = 0
    for i in 0..xs.len {
        total += xs[i]
    }
    return total
}

func main() int {
    values := [6]i64{1, 2, 3, 4, 5, 6}
    all := values[..]
    middle := values[2..4]
    return int(sum(all) + sum(middle))
}
```

Copying a slice copies those two words, so the elements stay shared. That is
the distinction to keep in mind: arrays are values, slices are views.

Indexing is checked. `xs[99]` on a six-element slice aborts with a message
naming the line, unless you compiled with `--mode=fast`.

## Strings

A `string` is an immutable slice of bytes, with literals living in read-only
memory:

```sword
import "std/io"

func main() !int {
    greeting := "hello, world\n"
    try io.Print(greeting)

    // .len is bytes, not characters
    if greeting.len != 13 {
        return 1
    }
    return 0
}
```

Indexing a string gives you bytes. Decoding UTF-8 is a library's job, not the
type's.

## Structs

```sword
struct Point {
    x i32
    y i32
}

func main() int {
    mut p := Point{x: 3, y: 4}
    p.x += 17
    return int(p.x + p.y)
}
```

Every field has to be given a value in a literal. There is no partial
initialisation, because a half-built struct is a bug waiting for a reader.

The compiler is free to reorder fields to remove padding:

```sword
struct Task {
    done bool
    id   u64
    prio u8
}
```

Written in that order in C this is 24 bytes. Sword lays it out as `id`, `done`,
`prio` and it comes to 16. If you need the declared order — because you are
handing the struct to C — write `extern struct`.

## Pointers and optionals

`*T` is a pointer, and it is never null. Absence lives in `?T`:

```sword
struct Node {
    key  i32
    next ?*Node
}

func find(head ?*Node, key i32) ?*Node {
    mut cur := head
    for {
        if n := cur {
            if n.key == key {
                return n
            }
            cur = n.next
        } else {
            return nil
        }
    }
}

func main() int {
    mut c := Node{key: 3, next: nil}
    mut b := Node{key: 2, next: &c}
    mut a := Node{key: 1, next: &b}

    hit := find(&a, 3) orelse return 1
    return int(hit.key) + 39
}
```

`?*Node` still fits in one word — the null pointer is the marker, so the safety
costs nothing. Over a non-pointer type, `?T` is a value and a flag beside it.

Two ways to get at the value, and no third:

- `if n := opt { ... }` runs the block with the unwrapped value bound
- `opt orelse fallback` produces the value or the fallback

`orelse` also takes a jump, which is the shape you will write most:

```sword
p := lookup(key) orelse return error.NotFound
```

## Errors

A function that can fail says so with `!` on its result:

```sword
func half(n i32) !i32 {
    if n % 2 != 0 {
        return error.NotEven
    }
    return n / 2
}
```

`error.NotEven` invents an error by naming it. Errors are just codes — they
carry no payload, never allocate, and the set is global to the program, so
there is no error type to declare anywhere.

`try` passes a failure up to your own caller:

```sword
func quarter(n i32) !i32 {
    a := try half(n)
    return try half(a)
}
```

`catch` handles it. Three shapes:

```sword
func half(n i32) !i32 {
    if n % 2 != 0 {
        return error.NotEven
    }
    return n / 2
}

func quarter(n i32) !i32 {
    a := try half(n)
    return try half(a)
}

func main() int {
    a := quarter(40) catch 0            // a fallback value

    b := quarter(41) catch |e| {        // inspect it, then leave
        return int(a) + 2
    }

    return int(a) + int(b)
}
```

and `catch {}` when you mean to carry on regardless — which is most of what you
write inside a `defer`.

A handler block that has to produce a value must leave the scope, with a
`return`, `break` or `continue`; there is nowhere for it to get one otherwise.
When the result is being thrown away anyway, as in a statement on its own, it
can just fall out.

You cannot ignore a failure by accident. Calling a fallible function and
throwing the result away is a compile error, and so is reaching into an `!T`
that has not been handled.

`main` can be fallible too, in which case an error that reaches it becomes the
exit status.

## defer

`defer` runs a statement when the block it sits in ends — the block, not the
function:

```sword
import "std/io"

func main() !int {
    try io.Print("a")
    {
        defer io.Print("c") catch {}
        try io.Print("b")
    }
    try io.Print("d\n")
    return 0
}
```

That prints `abcd`. Block scope is what makes `defer` inside a loop do the
obvious thing — release once per iteration, instead of piling up until the
function returns.

`errdefer` is the same, but only on the way out through an error. It is how you
undo half of something:

```sword
buf := try alloc(a, size)
errdefer a.release(buf, size)
try fill(buf)
return buf
```

Within a scope, deferred statements run in reverse order, and `defer` and
`errdefer` share that order.

## Methods and interfaces

A method is a function with a receiver:

```sword
struct Counter {
    hits u64
}

func (c *Counter) Value() u64 {
    return c.hits
}

func (mut c *Counter) Bump(by u64) {
    c.hits += by
}

func main() int {
    mut c := Counter{hits: 1}
    c.Bump(20)
    c.Bump(c.Value())
    return int(c.Value())
}
```

`mut` on the receiver means the same thing it means on any parameter, and only
a mutable binding can call such a method.

An interface is a set of method signatures, satisfied structurally — nothing is
declared to implement anything:

```sword
interface Shape {
    Area() i64
}

struct Square {
    side i64
}

func (s *Square) Area() i64 {
    return s.side * s.side
}

func describe(s Shape) i64 {
    return s.Area()
}

func main() int {
    mut sq := Square{side: 6}
    return int(describe(&sq)) + 6
}
```

Note the `&`. **Only pointers satisfy an interface.** Boxing a value would mean
allocating, and nothing in Sword allocates on its own, so the pointer is
explicit. An interface value is two words — the data and a table of methods —
and costs nothing to pass.

## Generics

```sword
func Max[T](a T, b T) T {
    if a > b {
        return a
    }
    return b
}

func main() int {
    x := Max(20, 22)
    y := Max[i32](1, 2)
    return int(x) + int(y) - 2
}
```

Each combination of types becomes a real compiled copy. There is no boxing and
no indirection. Type arguments are usually inferred from what you pass; write
them out when they cannot be.

Constraints are the same interfaces from the last section:

```sword
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

func total[T: Sized](a *T, b *T) u64 {
    return a.Size() + b.Size()
}

func main() int {
    mut p := Box{w: 3, h: 4}
    mut q := Box{w: 5, h: 6}
    return int(total(&p, &q))
}
```

One concept, two kinds of polymorphism. The difference is where the call is
resolved: through a constraint it is decided at instantiation and compiles to a
direct call, while an interface parameter goes through the method table at run
time.

Two builtins exist because a generic allocator cannot work them out for itself:
`sizeof[T]()` and `alignof[T]()`, both compile-time constants.

## Memory

There is no garbage collector and no hidden allocation. A function that needs
the heap takes an `Allocator`:

```sword
import "std/mem"

func main() !int {
    mut backing := [1024]u8{}
    mut arena := mem.NewArena(backing[..])

    mut xs := mem.Alloc[i64](&arena, 16) orelse return error.OutOfMemory
    for i in 0..xs.len {
        xs[i] = i64(i) * i64(i)
    }

    return int(xs[8])
}
```

`Arena` bumps a pointer through a buffer it does not own — here, an array on
the stack. Individual releases do nothing; you throw the whole arena away at
once, or call `Reset` and start over. That is the intended shape: an arena per
phase, or per task, rather than tracking objects one at a time.

When you want the system allocator instead, `mem.NewSystem()` gives you one
over `malloc` that tracks what is still out:

```sword
import "std/mem"

func main() !int {
    mut sys := mem.NewSystem()
    mut xs := mem.Alloc[i64](&sys, 4) orelse return error.OutOfMemory
    xs[0] = 42
    result := xs[0]
    mem.Free(&sys, xs)

    if sys.Live() != 0 {
        return 1
    }
    return int(result)
}
```

Both are values of type `Allocator` at the call site, so code written against
the interface works with either. Because an `Allocator` is a parameter and
never a global, a signature without one is a promise that the function does not
touch the heap — and the compiler has no way to break that promise.

## Packages

A package is a directory. Every `.sw` file in it shares one scope, so there are
no headers, no forward declarations, and no ordering rules:

```
myproject/
  main.sw
  shapes/
    shape.sw
    square.sw
```

In `main.sw`:

```sword
import "shapes"

func main() int {
    mut sq := shapes.NewSquare(6)
    return int(sq.Area()) + 6
}
```

A name is visible outside its package when it starts with a capital letter.
That applies to `extern` declarations too, so a package can wrap a C function
without exposing it.

Imports are searched for next to the file being compiled first, then wherever
the compiler keeps its standard library. Import cycles are a compile error.

Pointing the compiler at a single `.sw` file compiles that file alone — it will
not quietly pull in its neighbours. Point it at a directory to compile the
whole package.

## Tasks

This is what the language is for, so it has [a chapter of its
own](concurrency.md). The shape of it:

```sword
import "std/mem"

func fill(mut xs []i64, v i64) {
    for i in 0..xs.len {
        xs[i] = v
    }
}

func main() !int {
    mut backing := [2048]u8{}
    mut arena := mem.NewArena(backing[..])
    mut data := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory

    scope {
        for part in data.chunks(8) {
            spawn fill(part, 2)
        }
    }

    mut total i64 = 0
    for i in 0..data.len {
        total += data[i]
    }
    return int(total % 100)
}
```

Eight tasks, each writing its own piece. The closing brace of `scope` waits for
all of them. Nothing here is a convention you have to remember — the compiler
rejects the versions of this program that would race.

## Build modes

Bounds checks and overflow checks cost something, and the cost lands in the
loop you care about most. Rather than deciding for the whole language, the mode
decides per build:

```sh
shield program.sw --mode=debug    # checks, no optimisation
shield program.sw --mode=safe     # checks, optimised (the default)
shield program.sw --mode=fast     # no checks
shield program.sw --mode=small    # no checks, optimised for size
```

`+%`, `-%` and `*%` wrap on overflow and are never checked, in any mode. That
is how you ask for wrapping on purpose, in a hash or a PRNG.

The difference shows up in the generated code: a summing loop stays scalar in
`safe`, because the bounds check blocks it, and vectorises in `fast`. Same
source, no annotations.

## Looking inside

Every stage of the compiler will show you its work:

```sh
shield program.sw --emit-tokens
shield program.sw --emit-ast
shield program.sw --emit-ir      # Sword's own IR
shield program.sw --emit-llvm    # what gets handed to LLVM
```

`--emit-ir` is the one worth knowing about. It prints struct layouts with real
offsets, which is the quickest way to see what the field reordering did.

## Where to go next

[Concurrency](concurrency.md) is the rest of the story. The
[reference](reference.md) is for when you know what you want and need to
remember how to spell it.
