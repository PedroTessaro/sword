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
| `atomic[T]` | an integer or bool many tasks may write at once |
| `shared[T]` | any value, behind a mutex; `lock` is the only way in |
| `any` | a boxed value, what a `...any` parameter gathers |

`?*T` and `?[*]T` cost one word. Over anything else, `?T` is the value with a
flag beside it.

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

struct Name { field T ... }
struct Name[T] { field T ... }     // generic; a type only once instantiated
extern struct Name { ... }         // declared field order, for C
interface Name { Method(a T) R ... }

package name                       // documentation; the directory decides
import "path/to/package"
```

`mut` on a parameter or receiver means the callee may write through it. Only a
mutable binding can be passed to one.

A name is exported from its package when it starts with a capital letter.

## Statements

```sword
if cond { } else if cond { } else { }
if x := optional { } else { }

for { }                            // until break or return
for cond { }
for i in a..b { }
for part in xs.chunks(n) { }
for off, part in xs.chunks(n) { }

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
| `==` `!=` `<` `<=` `>` `>=` | |
| `&&` | short-circuits |
| `\|\|` | short-circuits |
| `catch` `orelse` | below everything else |

Prefix: `-` `!` `&` (address of) `*` (dereference) `try`.

Assignment: `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` and the
wrapping forms below.

`+%` `-%` `*%` and `+%=` `-%=` `*%=` wrap on overflow and are never checked, in
any build mode.

`[*]T` supports `p + n` and `p - n`. `*T` and `[]T` do not.

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
func f() !T                    // may fail
return error.Name              // fail with a code
return value                   // succeed
try expr                       // propagate a failure to the caller
expr catch fallback            // substitute a value
expr catch |e| { ... }         // handle it, naming the error
expr catch { ... }             // handle it without naming it
```

Error names are global to the program; each gets a code, and 0 always means
success. A `!T` cannot be discarded, and its fields cannot be reached until it
is handled.

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
```

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
func IndexByte(s string, c u8) u64
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

### `std/os`

```sword
func Argc() u64                   // including the program's own name
func Arg(i u64) string            // empty past the end
func Args(mut into []string) []string
func Env(name string) ?string     // nil when unset
func EnvOr(name string, fallback string) string
func Exit(code i32)               // no defers run, no output is flushed
```

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

### `std/net`

TCP over the loopback interface. Port 0 asks the operating system to choose.

```sword
func Listen(port i32) !Listener
func (l *Listener) Port() i32
func (l *Listener) Accept() !Conn
func (l *Listener) Close()                   // wakes a blocked Accept

func Dial(host string, port i32) !Conn
func DialTimeout(host string, port i32, limit time.Duration) !Conn
func (c *Conn) SetTimeout(limit time.Duration) !void  // 0 waits forever
func (c *Conn) Read(mut into []u8) !u64      // 0 means the peer is done
func (c *Conn) Write(from []u8) !void
func (c *Conn) WriteString(s string) !void
func (c *Conn) Close()
```

Every call here parks the thread in the kernel and tells the scheduler so, which
is what lets many more connections be in flight than the machine has cores. See
[Concurrency](concurrency.md#blocking-io).

### `std/http`

HTTP/1.1 with keep-alive, routing and timeouts. No TLS and no chunked transfer
encoding.

```sword
interface Handler {
    Serve(req *Request, mut res *Response) !void
}

func Listen(port i32) !Server
func (s *Server) Port() i32
func (s *Server) Serve(h Handler) !void      // 32 accept loops
func (s *Server) ServeWith(h Handler, workers int) !void
func (s *Server) Close()                     // stops a running server
```

A `Request` carries `Method`, `Target`, `Path`, `RawQuery`, `Proto`, `Headers`
and `Body`. `Target` is the request line unchanged; `Path` and `RawQuery` are
its two halves. The strings point into the connection's read buffer, so they are
valid for as long as the handler runs.

```sword
func (r *Request) Param(name string) string  // what a {name} route matched
func (r *Request) Query(name string) string  // still percent-encoded
```

Routing. A `{name}` in a pattern matches one path segment; a path that matched
under another method answers 405 rather than 404. Up to 64 routes, no
allocation:

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
func QueryValue(query string, name string) string
func Unescape(s string, mut into []u8) !string   // %20 and + become a space
func Escape(s string, mut into []u8) !string
```

```sword
func (mut r *Response) Printf(format string, args ...any) !void
func (mut r *Response) Text(status u64, s string) !void
func (mut r *Response) JSON(status u64, s string) !void
func (mut r *Response) SetHeader(name string, value string) !void
func (mut r *Response) Write(p []u8) !void
func (mut r *Response) WriteString(s string) !void
```

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

// The same through a default client.
func Get(host string, port i32, target string,
         mut a mem.Allocator) !ClientResponse
func Post(host string, port i32, target string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse
```

`Client` has two `time.Duration` fields, `Connect` and `Read`, either of which
may be zero to wait as long as the kernel would.

## Command line

```
shield <file.sw | directory> [options]

  -o <path>       output binary, default a.out
  -I <dir>        another directory to search for packages
  --mode=<m>      debug | safe | fast | small, default safe
  -O<level>       override the optimisation level
  --emit-tokens   stop after lexing
  --emit-ast      stop after parsing and checking
  --emit-ir       stop after lowering, print Sword IR
  --emit-llvm     print the LLVM IR
```

A `.sw` file compiles alone. A directory compiles as one package.

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

## Reserved but not implemented

`chan`. The word is taken; the feature is not there. There are also no function
values, so a callback is an interface with one method.
