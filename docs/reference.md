# Reference

For looking things up. [The tour](tour.md) is the place to learn from.

## Types

| | |
|---|---|
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `int` `uint` | word-sized, 64 bits |
| `f32 f64` | floating point — declarable, not yet usable in expressions |
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

`?*T` and `?[*]T` cost one word. Over anything else, `?T` is the value with a
flag beside it.

## Declarations

```sword
x := expr              // immutable, type inferred
mut x := expr          // mutable
x T = expr             // immutable, type written
mut x T = expr         // mutable, type written
```

```sword
func name(a T, mut b U) R { ... }
func name[T, U: Constraint](a T) R { ... }
func (r *T) Method() R { ... }
func (mut r *T) Method() R { ... }
extern func c_name(a T) R          // C ABI, name unchanged

struct Name { field T ... }
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

Assignment: `=` `+=` `-=` `*=` `/=` `%=` and the wrapping forms below.

`+%` `-%` `*%` and `+%=` `-%=` `*%=` wrap on overflow and are never checked, in
any build mode.

`[*]T` supports `p + n` and `p - n`. `*T` and `[]T` do not.

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

`T(x)` between numeric types, and between raw pointers. `*T` converts to
`[*]T`, never the other way — that would fabricate a non-null guarantee.

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

```sword
func Write(fd i32, s string) !u64      // bytes written
func Print(s string) !void             // to stdout
func Fail(s string) !void              // to stderr
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
| `SWORD_THREADS` | worker count, defaults to one per core |

## Reserved but not implemented

`const`, `chan`, `shared`. The words are taken; the features are not there.
