# Reference

For looking things up. [The tour](tour.md) is the place to learn from.

## Types

| | |
|---|---|
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `int` `uint` | word-sized, 64 bits |
| `f32 f64` | floating point |
| `bool` `void` | |
| `string` | immutable byte slice, two words |
| `error` | an error code, 16 bits |
| `*T` | pointer, never null |
| `?T` | optional; over a pointer it is the pointer, null meaning absent |
| `!T` | may fail: a `{ code, value }` pair |
| `[]T` | slice: pointer and length, two words |
| `[N]T` | array, a value |
| `[*]T` | raw pointer, no length, for FFI |
| `struct` `interface` | |
| `enum` | a named set of integers, with its own type |
| `func(T, mut U) R` | a function value: one word, no capture |
| `atomic[T]` | an integer or bool many tasks may write at once |
| `shared[T]` | any value, behind a mutex; `lock` is the only way in |
| `any` | a boxed value, what a `...any` parameter gathers |

`?*T`, `?[*]T` and `?func(...)` cost one word. Over anything else, `?T` is the
value with a flag beside it.

### String literals

Bytes between double quotes. The escapes, and there are no others:

| | |
|---|---|
| `\n` `\t` `\r` | newline, tab, carriage return |
| `\0` | a zero byte |
| `\e` | ESC, the same byte as `\x1b` |
| `\xNN` | the byte NN, two hex digits, either case |
| `\\` `\"` `\'` | the character itself |

`\xNN` is what a terminal sequence is written with, and it is what lets one be
a constant:

```sword
const ClearScreen = "\x1b[2J"
const Home = "\e[H"
```

## Declarations

```sword
x := expr              // immutable, type inferred
mut x := expr          // mutable
x T = expr             // immutable, type written
mut x T = expr         // mutable, type written

const Name = expr      // package level; folded at compile time
```

A constant's initialiser has to be literals, operators and other constants.
Declaration order does not matter, and a cycle is an error. A constant that is
an integer may be used as an array length.

```sword
func name(a T, mut b U) R { ... }
func name(a, b T) R { ... }        // one type covers the names before it
func name(a T, rest ...U) R { ... } // the last one gathers what is left
func name[T, U: Constraint](a T) R { ... }
func (r *T) Method() R { ... }
func (mut r *T) Method() R { ... }
extern func c_name(a T) R          // C ABI, name unchanged
extern func c_name(a T, ...) R     // C's own variadic: ioctl, fcntl, printf

struct Name { field T ... }
struct Name[T] { field T ... }     // generic; a type only once instantiated
extern struct Name { ... }         // declared field order, for C
interface Name { Method(a T) R ... }
enum Name T { A; B = 3; C }        // T is the width, default int
error Name = "what went wrong"     // declared, with the sentence it says
error Name                         // declared, saying nothing
error A, B, C

package name                       // documentation; the directory decides
import "path/to/package"
```

`mut` on a parameter or receiver means the callee may write through it. Only a
mutable binding can be passed to one.

A name is exported from its package when it starts with a capital letter. That
covers a struct's fields as well: a type can be public and its insides private,
which is why `time.Duration` has methods rather than a reachable `ns`.

## Statements

```sword
if cond { } else if cond { } else { }
if x := optional { } else { }

for { }                            // until break or return
for cond { }
for i in a..b { }
for x in xs { }                    // elements of a slice, array or string
for i, x in xs { }                 // with the position
for part in xs.chunks(n) { }
for off, part in xs.chunks(n) { }

switch subject { case a, b: ... default: ... }

parallel for i in a..b { }
parallel for i in a..b reduce(+: acc) { }

scope { spawn f(args) ... }
lock name := shared_value { }

break
continue
return expr
defer stmt
errdefer stmt
```

## Operators

By precedence, tightest first:

| | |
|---|---|
| `*` `/` `%` `<<` `>>` `&` | |
| `+` `-` `\|` `^` | |
| `==` `!=` `<` `<=` `>` `>=` | `==` on strings compares content |
| `&&` | short-circuits |
| `\|\|` | short-circuits |
| `catch` `orelse` | below everything else |

Prefix: `-` `!` `&` (address of) `*` (dereference) `try`.

Assignment: `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` and the
wrapping forms below.

`+%` `-%` `*%` and `+%=` `-%=` `*%=` wrap on overflow and are never checked, in
any build mode.

`[*]T` supports `p + n` and `p - n`. `*T` and `[]T` do not.

## Function values

A function named without being called is its address. There are no closures, so
a function value is one top-level function, one word wide, capturing nothing.

```sword
func apply(op func(i64, i64) i64, a i64, b i64) i64 {
    return op(a, b)
}

f := add                    // the function itself
mut maybe ?func() = nil     // one word, null meaning absent
```

Write `mut` on a parameter the function may write through: `func(mut []i64)` and
`func([]i64)` are different types. A generic function has no value — there is no
single function to point at. When a callback needs state alongside it, use an
interface: the state travels in the pointer.

## Variadic functions

The last parameter may gather the rest of the arguments, and inside the
function it is an ordinary slice:

```sword
func total(xs ...i64) i64
total(1, 2, 3)
total()                 // an empty slice
total(already...)       // pass a gathered list straight through
```

`...any` gathers a mixed list. An `any` carries the value together with a tag
saying what it is, filled in by the compiler from the static type at the call
site — there is no reflection anywhere. It is four fields:

| | |
|---|---|
| `Kind` | `u8`: 0 none, 1 bool, 2 int, 3 uint, 4 float, 5 string, 6 pointer |
| `Int` | signed, unsigned and bool values, widened to 64 bits |
| `Real` | floats, widened to `f64` |
| `Text` | strings |

`std/fmt` mirrors the tags as `KindBool`, `KindInt` and so on.

### C's variadic

An `extern` declaration may end in `...`, which is a different thing: it
gathers nothing, and says that the arguments after the declared ones travel by
the platform's rules.

```sword
extern func ioctl(fd i32, request u64, ...) i32
```

It matters. On Apple's arm64 a variadic argument goes on the stack while a
fixed one goes in a register, so declaring the arity you happen to use —
`ioctl(fd i32, request u64, p [*]u8)` — puts the third argument somewhere the
callee never looks. The same declaration passes on x86-64, which is the worst
way for it to be wrong.

What C promotes, Sword promotes: anything narrower than an `i32` arrives as
one, an `f32` as an `f64`. Only scalars fit through — a `string` is two words
here and a pointer there, so pass `.ptr` and `.len`. And a variadic function
can only be called by name: a function value carries one word and no
signature, which is not enough to know where the extra arguments go.

## Atomics

`atomic[T]` over an integer or a bool. It is the one thing several tasks may
write at the same time; the race checker lets it through and everything else
through only for reading.

```sword
mut hits := atomic[u64](0)

hits.Load()                  // T
hits.Store(v)
hits.Swap(v)                 // T, the previous value
hits.Add(v)                  // T, integers only
hits.Sub(v)
hits.And(v)
hits.Or(v)
hits.CompareSwap(old, new)   // bool: whether it matched
```

Every one of these is a single machine instruction, with sequentially
consistent ordering.

## Shared values

`shared[T]` over anything. Where an atomic covers one scalar, this covers a
whole structure — a map, a queue, a cache — by putting a mutex in front of it.

```sword
mut table := shared[collections.Map[u64]](try collections.NewMap[u64](&a, 64))

lock m := &table {
    seen := m.Get(key) orelse 0
    try m.Set(key, seen + 1)
}
```

`shared[T](v)` is the only way to build one and `lock` is the only way to reach
the value, which is what makes the race checker's exemption safe. Inside the
block the name is a mutable `*T`; the brace releases the lock, and so does a
`return`, a `break`, a `continue` or a `try` that fails.

Three operations beside `lock`, all called with the lock held:

```sword
func (s *shared[T]) Wait()        // let go, wait to be told, take it back
func (s *shared[T]) Notify()      // wake one waiter
func (s *shared[T]) NotifyAll()   // wake all of them
```

`Wait` is what turns a mutex into something a queue can be built out of — it is
what a channel is written on. A caller loops rather than testing once, because
what it waited for may be gone again by the time it wakes.

No `mut` is needed anywhere, the same way an atomic needs none: the type says it
will be written by whoever holds the lock, so the binding's mutability has
nothing left to say. A shared may also sit in a struct field, and the struct
around it stays ordinary.

The rules around it:

- a `lock` inside another on the same value is an error, and the runtime aborts
  with a message when the second one comes through a call;
- `spawn` under a lock whose `scope` is outside it is an error: the task would
  still be running when the block released. Put the `scope` inside the `lock`,
  or the `lock` inside the task.

There is no read-only lock and no try-lock. A contended lock spins briefly and
then sleeps, telling the scheduler while it waits, so a thread stuck behind a
long critical section does not cost a core.

## Switch

```sword
switch subject {
case a, b:
    ...
case c:
    ...
default:
    ...
}
```

The subject is an integer, a bool, a string or an enum. No fallthrough: a case
body ends where the next `case` begins. Two cases matching the same constant is
an error, and so is a second `default`.

A `switch` is not a loop, so `break` and `continue` inside one belong to the loop
around it.

Over an enum with no `default`, every member has to be covered:

```
error: this switch over Kind does not cover Bool, Int, Text
note: add the missing cases, or a 'default'
```

## Enums

```sword
enum Kind u8 {
    None            // 0
    Bool            // 1
    Text = 9
    Real            // 10
}
```

The width is optional and defaults to `int`. A member with no value continues
from the one before, starting at zero; two members with the same value is an
error.

A member is named through its type — `Kind.Text`, or `json.Kind.Object` from
another package. The type is its own, so nothing arithmetic reaches it by
accident:

| | |
|---|---|
| `==` `!=` | yes |
| `switch` | yes, and checked for completeness without a `default` |
| `+` `<` and the rest | no |
| `u8(k)`, `Kind(n)` | yes, and **not** checked against the members |

`nameof(k)` is the name of the member `k` is, and the empty string when it is not
one. It works on an error too. Printing an enum gives the name for the same reason, so the number takes a
conversion:

```sword
try io.Printf("{} is {}\n", k, u8(k))    // Real is 10
```

## Reductions

```sword
parallel for i in a..b reduce(op: acc) { ... }
```

`op` is `+`, `&`, `|`, `min` or `max`; only `+` applies to a float. The body
combines one element into the worker's private copy — `acc += xs[i]` for a sum,
`if xs[i] > acc { acc = xs[i] }` for a maximum — and the clause says how the
copies are folded together at the end.

## Errors

```sword
error Name = "what went wrong"  // declared at the top level, message optional
error A, B, C

func f() !T                    // may fail
return error.Name              // fail with a code
return value                   // succeed
try expr                       // propagate a failure to the caller
expr catch fallback            // substitute a value
expr catch |e| { ... }         // handle it, naming the error
expr catch { ... }             // handle it without naming it
```

`nameof(e)` gives an error's name and `e.Message()` the sentence it was declared
with — the compiler writes both tables, because at run time an error is a code and
nothing else. A code that is not an error, zero among them, answers the empty
string for both. An error declared without a message has an empty one.

Errors are **declared**, and using one that is not is a compile error. That is the
point of the declaration: `error.Tiemout` used to compile and become a second
error nobody handled.

Codes are global to the program, so the same name raised in two packages is one
error and a caller handles it once — but the two declarations have to agree on the
message, and the compiler says so if they do not. A lowercase name is private to
the package that declared it, like everything else. A `!T` cannot be discarded, and
its fields cannot be reached until it is handled.

The message is a string literal, so it costs no allocation: it sits in the binary
and an error that goes unhandled costs nothing to carry. That is why there is no
`errors.New` taking a formatted string — building a message is the caller's job,
at the point where it has somewhere to put it.

A handler block that has to produce a value must leave the scope. In statement
position nothing is expected of it.

`func main() !int` and `func main() !void` both work; an error that reaches
`main` becomes the exit status.

## Optionals

```sword
var ?*T = nil
if x := optional { ... } else { ... }
optional orelse fallback
optional orelse return error.Name
optional == nil                // present or not
optional != nil
```

Comparing against `nil` is the only comparison an optional has: two of them
would mean comparing payloads, and an absent one has none.

## Conversions

`T(x)` between numeric types, between raw pointers, and between `string` and
`[]u8` in either direction. `*T` converts to `[*]T`, never the other way — that
would fabricate a non-null guarantee.

`p[a..b]` on a `[*]T`, with both bounds written, produces a `[]T`. It is the
crossing from unchecked memory to checked.

## Members

| | |
|---|---|
| `xs.len` | slices, arrays, strings |
| `xs.ptr` | slices and strings, as `[*]T` |
| `s.field` | struct fields, auto-dereferencing through `*T` |
| `x.Method()` | methods, and interface dispatch |

## Generics

```sword
func F[T](x T) T
func F[T: Constraint](x *T) R
func (v *Box[T]) Method() T        // methods on a generic struct
F(value)               // type argument inferred
F[i32](value)          // written out
sizeof[T]()            // compile-time constant
alignof[T]()
```

Constraints are interfaces. A call through a constraint is resolved when the
function is instantiated and compiles to a direct call; a call through an
interface parameter goes via the method table.

Type arguments written out have to be type names. Composite types come from
inference.

## Standard library

### `std/io`

Printing takes a fixed block of stack and flushes as it fills, so none of it
needs an allocator.

```sword
func Print(s string) !void                              // stdout
func Fail(s string) !void                               // stderr
func Write(fd i32, s string) !u64                       // bytes written

func Printf(format string, args ...any) !void
func Errorf(format string, args ...any) !void
func Fprintf(fd i32, format string, args ...any) !void
func Println(args ...any) !void                         // spaced, newline

// A Writer is a buffered sink over a descriptor; it satisfies fmt.Sink.
func NewWriter(fd i32) Writer
func (mut w *Writer) WriteByte(c u8) !void
func (mut w *Writer) Write(p []u8) !void
func (mut w *Writer) Flush() !void
```

### `std/mem`

```sword
interface Allocator {
    Alloc(n u64, align u64) ?[*]u8
    Release(p [*]u8, n u64)
}

func Align(offset u64, align u64) u64

func Alloc[T](mut a Allocator, n u64) ?[]T
func Free[T](mut a Allocator, xs []T)
```

`Arena` — bumps through a buffer it does not own:

```sword
func NewArena(backing []u8) Arena
func (mut a *Arena) Alloc(n u64, align u64) ?[*]u8
func (mut a *Arena) Release(p [*]u8, n u64)     // does nothing
func (mut a *Arena) Reset()
func (a *Arena) Used() u64
```

`Arena.Watch` is a field: set it and `Reset` writes `mem.Poison` (0xDE) over
everything it takes back, so anything still holding memory from before the reset
reads something obviously wrong. `shield test` sets it on every test's arena.
`Watched` does the same for memory released one piece at a time:

```sword
func NewWatched(a Allocator) Watched            // { inner, Written }
func (mut w *Watched) Alloc(n u64, align u64) ?[*]u8
func (mut w *Watched) Release(p [*]u8, n u64)   // writes over it first
```

Both cost a write per byte released, so neither is on by default. See [the tour on
memory that has gone away](tour.md#memory-that-has-gone-away).

`System` — over C `malloc` and `free`:

```sword
func NewSystem() System
func (mut s *System) Alloc(n u64, align u64) ?[*]u8
func (mut s *System) Release(p [*]u8, n u64)
func (s *System) Live() u64
```

An allocator is not safe to share between tasks, and the race checker says so:
handing the same one to two `spawn`s is a compile error. Give each task its own.

### `std/bytes`

A buffer that grows, holding the allocator it was built with:

```sword
func New(mut a mem.Allocator, capacity u64) !Buffer
func (b *Buffer) Len() u64
func (b *Buffer) Bytes() []u8
func (b *Buffer) Str() string
func (mut b *Buffer) Reset()
func (mut b *Buffer) Free()
func (mut b *Buffer) WriteByte(c u8) !void
func (mut b *Buffer) Write(from []u8) !void
func (mut b *Buffer) WriteString(s string) !void
func (mut b *Buffer) WriteU64(v u64) !void
```

### `std/strings`

Byte-oriented, which is what a protocol parser wants. `IndexByte` and `Index`
return the length of the haystack when there is no match.

```sword
func Equal(a string, b string) bool
func EqualFold(a string, b string) bool      // case-insensitive
func HasPrefix(s string, prefix string) bool
func HasSuffix(s string, suffix string) bool
func IndexByte(s string, c u8) u64
func LastIndexByte(s string, c u8) u64
func Index(s string, needle string) u64
func Contains(s string, needle string) bool
func TrimSpace(s string) string
func ToLower(c u8) u8
func ParseU64(s string) !u64
```

### `std/collections`

```sword
func NewList[T](mut a mem.Allocator, capacity u64) !List[T]
func (l *List[T]) Len() u64
func (l *List[T]) Cap() u64
func (l *List[T]) Slice() []T          // invalid after the next Push
func (l *List[T]) At(i u64) T
func (mut l *List[T]) Set(i u64, v T)
func (mut l *List[T]) Push(v T) !void
func (mut l *List[T]) Pop() ?T
func (mut l *List[T]) Reset()
func (mut l *List[T]) Free()
```

A hash map with string keys, open addressing and linear probing:

```sword
func NewMap[V](mut a mem.Allocator, capacity u64) !Map[V]
func Hash(key string) u64
func (m *Map[V]) Len() u64
func (m *Map[V]) Get(key string) ?V
func (m *Map[V]) Has(key string) bool
func (mut m *Map[V]) Set(key string, value V) !void
func (mut m *Map[V]) Delete(key string) bool
func (mut m *Map[V]) Free()

// Iteration is by slot: walk 0..Slots() and ask each one.
func (m *Map[V]) Slots() u64
func (m *Map[V]) KeyAt(i u64) ?string
func (m *Map[V]) ValueAt(i u64) V
```

Keys are strings. A key type parameter would need hashing and equality as
constraints, and there is nowhere to hang those yet.

### `std/fmt`

Everything writes into a sink rather than one particular buffer, so the same
code serves a growable buffer, a socket, or a block of stack.
`bytes.Buffer` and `io.Writer` both satisfy it without being told to.

```sword
interface Sink {
    WriteByte(c u8) !void
    Write(p []u8) !void
}

func Format(mut out Sink, format string, args []any) !void
func Sprintf(mut a mem.Allocator, format string, args ...any) !string
func Value(mut out Sink, v any) !void

func Str(mut out Sink, s string) !void
func U64(mut out Sink, v u64) !void
func I64(mut out Sink, v i64) !void
func Bool(mut out Sink, v bool) !void
func Hex(mut out Sink, v u64, width u64) !void
func F64(mut out Sink, v f64, decimals u64) !void
func Float(mut out Sink, v f64) !void     // up to six places, zeros trimmed
func Pad(mut out Sink, s string, width u64) !void
func Quote(mut out Sink, s string) !void  // JSON string, escaped
```

The format language is small on purpose:

| | |
|---|---|
| `{}` | the next argument, formatted by what it is |
| `{x}` | an integer in hexadecimal |
| `{.N}` | a float with N decimal places |
| `{{` `}}` | a literal brace |

### `std/json`

A parsed document is one flat list of nodes; children are reached by index
rather than by pointer, so the whole tree is a single allocation.

```sword
func Parse(input []u8, mut a mem.Allocator) !Document

func (d *Document) Root() u64
func (d *Document) Kind(at u64) u8         // Null Bool Number Str Array Object
func (d *Document) Text(at u64) string
func (d *Document) Number(at u64) f64
func (d *Document) Truth(at u64) bool
func (d *Document) Len(at u64) u64         // array or object size
func (d *Document) At(at u64, i u64) ?u64
func (d *Document) Get(at u64, key string) ?u64
func (d *Document) KeyAt(at u64) string
func (d *Document) GetText(at u64, key string) string
func (d *Document) GetNumber(at u64, key string, fallback f64) f64
```

Writing is a streaming writer that puts the commas in for you:

```sword
mut w := json.NewWriter(&buffer)
try w.BeginObject()
try w.Key("name")
try w.Str("sword")
try w.Key("tags")
try w.BeginArray()
try w.Str("fast")
try w.EndArray()
try w.EndObject()
```

### `std/crypto`

Nothing here allocates. A hasher is a value and a digest is an array of bytes,
so hashing inside a request handler costs no arena and no allocator parameter.

```sword
const Sha256Size = 32

func Sha256Of(data []u8) [Sha256Size]u8    // the whole message at once

func NewSha256() Sha256
func (mut h *Sha256) Write(p []u8)         // any amount at a time
func (mut h *Sha256) Sum() [Sha256Size]u8
```

`Write` keeps whatever does not fill a block, so a file can be hashed as it is
read rather than after it is read:

```sword
mut h := crypto.NewSha256()
mut buf := [32768]u8{}
for {
    n := try file.Read(buf[0..32768])
    if n == 0 {
        break
    }
    h.Write(buf[0..n])
}
digest := h.Sum()
```

`Sum` finishes the message. The padding it writes is part of what was hashed,
so there is nothing sensible to append afterwards: a hasher is spent once its
digest has been taken, and another message needs another hasher.

### `std/os`

```sword
func Argc() u64                   // including the program's own name
func Arg(i u64) string            // empty past the end
func Args(mut into []string) []string
func Env(name string) ?string     // nil when unset
func EnvOr(name string, fallback string) string
func Exit(code i32)               // no defers run, no output is flushed

enum Signal i32 { Hangup = 1, Interrupt = 2, Quit = 3, Terminate = 15 }

func Catch(sig Signal) !void      // start catching it
func WaitSignal() !Signal         // the next one, without holding a thread
func Kill(pid u64, sig Signal) !void

func MaxFiles() u64               // descriptors this process may open
func RaiseMaxFiles(want u64) !u64 // 0 asks for the hard limit
func Pid() u64
func Cpus() u64
func Hostname() ?string
```

A signal handler may do almost nothing safely, so a signal arrives down a pipe
and `WaitSignal` is an ordinary task parked on the other end. That is the whole
of a graceful shutdown: wait, call `Close`, let the scope drain.

`Args` fills an array you supply instead of allocating, and returns the part of
it that was used:

```sword
mut room := [8]string{}
args := os.Args(room[..])
```

### `std/time`

```sword
func Nanos(n i64) Duration
func Micros(n i64) Duration
func Millis(n i64) Duration
func Seconds(n i64) Duration

func (d Duration) AsNanos() i64
func (d Duration) AsMicros() i64
func (d Duration) AsMillis() i64
func (d Duration) AsSeconds() f64
func (d Duration) Add(other Duration) Duration
func (d Duration) Less(other Duration) bool

func Now() Instant                // monotonic: measure with this one
func Since(start Instant) Duration
func (t Instant) Add(d Duration) Instant
func (t Instant) Until() Duration // negative once it is past

func Unix() i64                   // wall clock seconds: stamp with this one
func UnixNanos() i64
func Sleep(d Duration)            // the scheduler is told first
```

A method needs a value with an address, so `time.Since(start).AsMillis()` does
not compile — bind the duration first.

### `std/testing`

Tests live in `*_test.sword` beside the code and are found by name: `Test...`
taking one `*T`. See [Testing](testing.md).

```sword
func (t *T) Name() string
func (t *T) Failed() bool

// These record and carry on.
func (mut t *T) Same(got any, want any) !void
func (mut t *T) Check(ok bool) !void
func (mut t *T) Failf(format string, args ...any) !void

// These end the test, by returning an error the `try` carries out.
func (mut t *T) Equal(got any, want any) !void
func (mut t *T) Require(ok bool) !void
func (mut t *T) Fatalf(format string, args ...any) !void

func (mut t *T) Logf(format string, args ...any) !void
func (mut t *T) Skip(why string) !void
func (mut t *T) Run(name string, body func(mut *T) !void) !void
func (mut t *T) RunWith(name string, body func(mut *T) !void, arg any) !void
```

Tests run at the same time, 64 at once by default; `shield test <path> -p 1`
puts them back in order. `t.Mem` is an arena of that test's own. `t.Arg` is what `RunWith` handed the
subtest, since a body cannot capture anything.

### Channels

A queue tasks hand values through, and part of the language rather than a package
to import. See [Concurrency](concurrency.md#channels).

```sword
mut ch := try chan[T](&arena, capacity)  // capacity 0 is a handover
try ch <- v                              // waits while full; fails once closed
v := <-ch                                // ?T: waits while empty, nil once drained
for v := <-ch { }                        // until it closes
close(ch)                                // twice is harmless
```

`<-` is one token, so `a < -b` needs the space.

```sword
select {
case v := <-ch:      // v is ?T — nil when the channel closed
case <-ch:           // the same, thrown away
case ch <- value:    // when there is room
default:             // when none of them could go
}
```

At most one `default`, and a select with no cases at all is a compile error. A
receive case is ready when the channel has a value *or* is closed; a send case is
ready when there is room, and never on a closed channel. Cases are tried from a
rotating start, so one that is always ready cannot starve the rest. A select whose
only remaining cases send into closed channels stops the program rather than
waiting for what cannot happen.

The rest are ordinary methods:

```sword
func (c Chan[T]) TrySend(v T) bool      // false rather than a wait
func (c Chan[T]) TryRecv() ?T
func (c Chan[T]) Len() u64
func (c Chan[T]) Cap() u64              // 0 for a handover
func (c Chan[T]) Closed() bool
func (c Chan[T]) Free(mut a mem.Allocator)
```

A channel is a handle: copying one copies the handle, both copies are the same
channel, and it goes to a task by value. Its memory comes from the allocator you
pass, which is why there is no `make`.

No receiver is `mut`, because every task that shares a channel reaches it at once
and the state behind it is a `shared` value.

### `std/fs`

```sword
enum Mode i32 { Read, Write, Append }   // Write truncates or creates

func Open(path string, mode Mode) !File
func Of(fd i32) File                    // wrap one the process already has
func (f *File) Read(mut into []u8) !u64 // 0 means the end of the file
func (f *File) Write(from []u8) !void
func (f *File) WriteString(s string) !void
func (f *File) Close()

func Size(path string) ?u64             // nil when absent, or a directory
func Exists(path string) bool
func Remove(path string) !void
func MakeDir(path string) !void               // content if it already exists
func RemoveDir(path string) !void             // an empty one only
func ReadAll(path string, mut a mem.Allocator) ![]u8
func WriteAll(path string, data []u8) !void
func ReadStdin(mut a mem.Allocator, most u64) ![]u8
```

`Stdin`, `Stdout` and `Stderr` are the descriptors the process starts with.

A file is never "not ready yet" — the wait is the disk, and no poller has
anything to say about it. So a file call goes to a thread kept for exactly that,
and the task waiting on it is put down like any other. Forty tasks reading at once
run on a pool of ten, not forty threads; `SWORD_IO_THREADS` is the cap and
`runtime.Read().IoThreads` is the count.

### `std/runtime`

What the scheduler is doing, for whoever runs the program rather than writes it.
A few relaxed loads, so it is cheap enough to read on a timer or to answer a
request with.

```sword
func Read() Stats
func (s Stats) Running() i64      // Started - Finished
```

| Field | |
|---|---|
| `Threads` | workers, including any hired to cover one stopped in the kernel |
| `Queued` | tasks spawned and not yet picked up — a queue that keeps growing is the first sign of a server taking more than it can serve |
| `Parked` | threads stopped inside a syscall that cannot be put down |
| `Stacks` | task stacks alive, in use or pooled |
| `Started` `Finished` | tasks begun and ended since the program did |
| `StackBytes` | what one task's stack reserves, which `SWORD_STACK_KB` sets |
| `IoThreads` | threads kept for calls that cannot be put down, capped by `SWORD_IO_THREADS` |

### `std/net`

TCP over the loopback interface. Port 0 asks the operating system to choose.

```sword
func Listen(port i32) !Listener                        // loopback only
func ListenOn(host string, port i32, share bool) !Listener
func (l *Listener) Port() i32
func (l *Listener) Accept() !Conn
func (l *Listener) Close()                   // wakes a blocked Accept

func Dial(host string, port i32) !Conn
func DialTimeout(host string, port i32, limit time.Duration) !Conn
func (c *Conn) SetTimeout(limit time.Duration) !void  // any one wait
func (c *Conn) SetDeadline(at time.Instant) !void     // the whole exchange
func (c *Conn) ClearDeadline() !void
func (c *Conn) Read(mut into []u8) !u64      // 0 means the peer is done
func (c *Conn) Write(from []u8) !void
func (c *Conn) WriteString(s string) !void
func (c *Conn) Peer(mut into []u8) !Peer   // { Address, Port }
func (c *Conn) Close()
```

A wait here puts the *task* down, not the thread: the descriptor goes to the
poller and the worker moves on. That is what lets connections outnumber threads
by a couple of orders of magnitude. See [Concurrency](concurrency.md#waiting).

`Stream` is what a socket satisfies without being asked, and what `std/http` is
written against, so a server serves a plain socket and a TLS connection with the
same code:

```sword
interface Stream {
    Read(mut into []u8) !u64
    Write(from []u8) !void
    WriteString(s string) !void
    SetTimeout(limit time.Duration) !void
    SetDeadline(at time.Instant) !void
    Peer(mut into []u8) !Peer
    Close()
}

func (c *Conn) Fd() i32                      // for a layer that wraps the socket
```

### `std/tls`

TLS 1.2 and 1.3 over a socket, from OpenSSL. Optional: the build looks for OpenSSL
in the usual places (`SWORD_OPENSSL=<prefix>` names another, `SWORD_NO_TLS=1` skips
it), and a build without it answers `Available() == false` and fails every call
here rather than failing to link.

```sword
func Available() bool

func NewConfig() Config              // { CAFile, Verify, CertFile, KeyFile }
func ClientContext(c Config) !Context        // one context, any number of conns
func ServerContext(certFile string, keyFile string) !Context

enum Ask u8 { Nobody, Required, Optional }   // what a server asks of a client
func NewServerConfig(certFile string, keyFile string) ServerConfig
func ServerContextWith(c ServerConfig) !Context   // { CertFile, KeyFile, ClientCA, Clients }
func (c *Context) Free()

func Dial(ctx *Context, host string, port i32) !Conn
func Client(ctx *Context, socket net.Conn, host string) !Conn
func Server(ctx *Context, socket net.Conn) !Conn

func (c *Conn) Read(mut into []u8) !u64
func (c *Conn) Write(from []u8) !void
func (c *Conn) WriteString(s string) !void
func (c *Conn) SetTimeout(limit time.Duration) !void
func (c *Conn) SetDeadline(at time.Instant) !void
func (c *Conn) Peer(mut into []u8) !net.Peer
func (c *Conn) Version() string              // "TLSv1.3"
func (c *Conn) Cipher() string
func (c *Conn) PeerName(mut into []u8) ?string  // the peer's certificate subject
func (c *Conn) Verified() bool               // it presented one and it checked out
func (c *Conn) Close()

// A certificate signed by its own key, valid for a day, and marked as an authority
// so it can sign others. Development and tests.
func SelfSigned(host string, certFile string, keyFile string) !void
// One signed by that authority instead of by itself, so two of them can check each
// other. `forClient` marks it for client authentication.
func SignedBy(name string, caCert string, caKey string, certFile string,
              keyFile string, forClient bool) !void
```

`Ask.Required` refuses a client without a certificate signed by `ClientCA`, and is
the only setting under which a handler may believe `PeerName`. `Ask.Optional` lets
both kinds through and leaves the checking to the handler. Asking for a certificate
with no `ClientCA` to check it against is an error, not a default.

A `Conn` keeps the socket it took over in `Socket`, so deadlines and the peer's
address stay where they were, and it is a `net.Stream`. The handshake and every
read and write put the *task* down while the socket is not ready, the same as a
plain read — twenty handshakes at once are twenty tasks, not twenty threads.

`Config.Verify` off means encryption with no idea who is on the other end. It is
there for talking to your own machine and it is not security.

### `std/http`

HTTP/1.1 with keep-alive, routing, timeouts, chunked transfer encoding, static
files and TLS.

```sword
interface Handler {
    Serve(req *Request, mut res *Response) !void
}

func Listen(port i32) !Server                        // loopback only
func ListenOn(host string, port i32, share bool) !Server
func ListenTLS(port i32, certFile string, keyFile string) !Server
func ListenOnTLS(host string, port i32, share bool, certFile string,
                 keyFile string) !Server
func ListenWith(port i32, conf tls.ServerConfig) !Server      // asks for a client cert
func ListenOnWith(host string, port i32, share bool, conf tls.ServerConfig) !Server
func (s *Server) Secure() bool               // whether it handshakes first
func (mut s *Server) Free()                  // gives the certificate back
func (s *Server) Port() i32
func (s *Server) Serve(h Handler) !void      // up to 1024 connections at once
func (s *Server) ServeWith(h Handler, most u64) !void
func (s *Server) Live() u64                  // connections in flight
func (s *Server) Accepted() u64              // connections taken, in total
func (s *Server) Served() u64                // requests answered
func (s *Server) Failed() u64                // of those, handlers that failed
func (s *Server) Close()                     // stops accepting, then drains
```

A `Request` carries `Method`, `Target`, `Path`, `RawQuery`, `Proto`,
`RemoteAddr`, `PeerName`, `Headers` and `Body`. `PeerName` is the subject of the
client's certificate when the server asked for one and got a valid one, and empty
otherwise — including every request under `Ask.Optional` that arrived without one. A body sent with `Transfer-Encoding:
chunked` is decoded before the handler sees it. `Target` is the request line unchanged; `Path` and `RawQuery` are
its two halves. The strings point into the connection's read buffer, so they are
valid for as long as the handler runs.

```sword
func (r *Request) Param(name string) string  // what a {name} route matched
func (r *Request) Query(name string) string  // still percent-encoded
```

Routing. A `{name}` in a pattern matches one path segment and a `{name...}` at the
end matches the rest of the path, slashes and all — including none of it, so
`/files/{path...}` routes `/files/` too. A path that matched under another method
answers 405 rather than 404. Up to 64 routes, no allocation:

```sword
func NewMux() Mux
func (mut m *Mux) Handle(method string, pattern string, h Handler) !void
func (mut m *Mux) Get(pattern string, h Handler) !void
func (mut m *Mux) Post(pattern string, h Handler) !void
func (mut m *Mux) Put(pattern string, h Handler) !void
func (mut m *Mux) Delete(pattern string, h Handler) !void
```

Targets and query strings, decoding into space you provide:

```sword
func ParseTarget(target string) Target       // { Path, RawQuery }
func ParseURL(text string, fromHost string, fromPort i32) !URL  // { Host, Port, Target, TLS }
func QueryValue(query string, name string) string
func Unescape(s string, mut into []u8) !string   // %20 and + become a space
func Escape(s string, mut into []u8) !string
```

```sword
func (mut r *Response) Stream(status u64) !void   // switch to chunked
func (mut r *Response) Send(status u64, length u64) !void  // a length you know
func (mut r *Response) Printf(format string, args ...any) !void
func (mut r *Response) Text(status u64, s string) !void
func (mut r *Response) JSON(status u64, s string) !void
func (mut r *Response) SetHeader(name string, value string) !void
func (mut r *Response) Write(p []u8) !void
func (mut r *Response) WriteString(s string) !void
```

Both `Stream` and `Send` put the head out at once, so headers set after either are
too late, and every write from then on goes to the connection rather than into a
buffer. After `Send`, write exactly `length` bytes.

Files. A piece at a time against the length the file already has, so a large
download costs one buffer:

```sword
func ServeFile(mut res *Response, path string) !void
func NewFiles(root string) Files             // { Root, Index, Param }
func (f *Files) Serve(req *Request, mut res *Response) !void
func MimeType(path string) string            // by extension
```

`Files` serves what a `{path...}` route caught, under its root. A path containing
`..`, a NUL or a backslash is refused rather than resolved. A directory gets
`Index` — `index.html` by default, and an empty `Index` makes directories 404.
Only GET and HEAD; anything else is a 405.

Header lookup is case-insensitive. A request carries at most 32 headers and
16 KiB of head; beyond that the connection is rejected. An accepted connection
gets a 15-second timeout, so a client that connects and says nothing releases
its accept loop rather than holding it.

The client returns a response whose body lives in the allocator you pass in:

```sword
func NewClient() Client                      // 10s connect, 30s read
func (c *Client) Get(host string, port i32, target string,
                     mut a mem.Allocator) !ClientResponse
func (c *Client) Post(host string, port i32, target string,
                      contentType string, body []u8,
                      mut a mem.Allocator) !ClientResponse
func (c *Client) Do(host string, port i32, method string, target string,
                    contentType string, body []u8,
                    mut a mem.Allocator) !ClientResponse

// Over TLS. Fetch takes a URL, which is the only shape that can say a scheme.
func (c *Client) Fetch(url string, mut a mem.Allocator) !ClientResponse
func (c *Client) GetTLS(host string, port i32, target string,
                        mut a mem.Allocator) !ClientResponse
func (c *Client) PostTLS(host string, port i32, target string,
                         contentType string, body []u8,
                         mut a mem.Allocator) !ClientResponse
func (c *Client) DoTLS(host string, port i32, method string, target string,
                       contentType string, body []u8,
                       mut a mem.Allocator) !ClientResponse

// The same through a default client.
func Get(host string, port i32, target string,
         mut a mem.Allocator) !ClientResponse
func Post(host string, port i32, target string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse
func Fetch(url string, mut a mem.Allocator) !ClientResponse
```

`Client` has two `time.Duration` fields, `Connect` and `Read`, either of which
may be zero to wait as long as the kernel would, and `Redirects`, how many to
follow — five by default, zero to hand the 3xx back. 301, 302 and 303 become a
GET; 307 and 308 keep the method and body.

```sword
func (mut c *Client) Close()            // lets go of the kept connection
```

A chunked reply is decoded, and a reply with no framing at all is read until the
connection closes. `Client.TLS` is a `tls.Config` and decides how an `https` URL is
trusted.

A client **keeps the connection it used**, with its TLS session, and uses it again
for the next request to the same host and port. That is why the methods take `mut`
and why a client belongs to one task. `Close()` matters: a kept connection holds a
task on the server. A request that fails on the kept connection is retried on a
fresh one for `GET` and `HEAD`, and comes back as `error.Interrupted` otherwise —
the request may already have been carried out.

## Command line

```
shield <file.sword | directory> [options]
shield test <file.sword | directory> [options]

  -o <path>       output binary, default a.out
  -I <dir>        another directory to search for packages
  --link <arg>    an object, a library or a linker option; one per --link
  --mode=<m>      debug | safe | fast | small, default safe
  -O<level>       override the optimisation level
  -p <n>          test only: how many tests may run at once
  -run <name>     test only: run just this test; repeat for more
  --emit-tokens   stop after lexing
  --emit-ast      stop after parsing and checking
  --emit-ir       stop after lowering, print Sword IR
  --emit-llvm     print the LLVM IR
```

A `.sword` file compiles alone. A directory compiles as one package.

`--link` reaches the linker, which is how a program calls C that `extern` alone
cannot describe — a function taking a struct whose layout differs between
systems, say. One argument each, in the order given, after the program's own
object:

```
shield sheath.sword -o sheath --link shim.o --link -L/opt/homebrew/lib \
    --link -lsomething
```

`shield test` builds the package together with its `*_test.sword` files behind a
generated entry point, runs it, and hands back its exit status. Those files are
left out of every other build. See [Testing](testing.md).

| Mode | Bounds and overflow checks | Optimisation |
|---|---|---|
| `debug` | yes | none |
| `safe` | yes | full |
| `fast` | no | full |
| `small` | no | for size |

## Environment

| | |
|---|---|
| `SWORD_ROOT` | where the compiler looks for the standard library |
| `SWORD_THREADS` | fixed worker count, defaults to one per core |
| `SWORD_MAX_THREADS` | how far the pool may grow to cover blocked tasks, default 512 |
| `SWORD_STACK_KB` | stack per task in KiB, default 1024, held between 64 and 262144 |
| `SWORD_IO_THREADS` | threads for file I/O and name resolution, default the core count or 4 |

## Reserved but not implemented

Nothing, now that `chan` is a package rather than a keyword.
