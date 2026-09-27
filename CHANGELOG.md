# Changes

Releases are numbered by [semantic versioning](https://semver.org): until 1.0,
a minor version may change the language, and each one says how. Each release is
also named for a step in making a blade, in the order they happen.

## 0.1.0 — Tamahagane

*2026-09-27.* The raw steel: the first release, and everything the language is
built from.

### The language

- Go's syntax, no garbage collector, and nothing that allocates without being
  handed an `Allocator` — a signature without one cannot touch the heap.
- Immutable by default; `mut` belongs to the binding, not the type.
- Errors are values that cannot be ignored: `!T`, `try`, `catch`, and errors
  declared with the sentence they say. Optionals are `?T` with `orelse`.
- Structs, enums with exhaustive `switch`, interfaces, generics, slices that
  know their length, `defer`, packages with the uppercase rule for what is
  exported.
- Bounds and integer overflow checked in the default build; `--mode=fast` and
  `--mode=small` for when they have been paid for.
- Two constant strings join with `+` at compile time; at run time,
  `strings.Concat` takes the allocator the result goes in.

### Concurrency

- `scope` and `spawn`: a task cannot outlive the block that started it, and
  everything it can reach is on the line that starts it.
- The compiler refuses a program where two tasks can touch the same memory,
  unless both only read or the pieces come from one `chunks` partition.
  `atomic[T]` and `shared[T]` are the two ways to share on purpose.
- Every task has its own stack, and a wait on a socket, a timer, a file or a
  name lookup puts the task down, not the thread. Measured on ten cores: 4000
  concurrent keep-alive connections at 84 000 requests a second on 12 threads,
  and 2000 idle connections in 65 MiB.
- `chan[T]`, `<-`, `close` and `select`.
- `parallel for` spreads a loop over every core, with `reduce` for the
  accumulators.

### The same answer, however many threads

- `det` on a function is a claim the compiler checks: what it answers depends on
  its arguments and nothing else. A `scope`, a `parallel for`, and a pipeline of
  tasks joined by channels with one sender and one receiver each are allowed
  inside it; an atomic, a lock, the clock or an address read as a number are
  not, and the error walks the chain of calls to the line that broke it.
- A reduction answers the same bits at any thread count, floating point
  included, and a scope reports the error of the earliest task that failed.
- `std/rand`: random numbers where the i-th depends on the seed and i (Philox).
- `std/par`: a prefix sum, a filter that keeps the order, the position of the
  smallest element, and a stable sort, all under the same rule.
- `std/num`: `Sqrt`, `Exp`, `Log`, `Sin` and `Cos` with the same bits on every
  machine and within one unit in the last place, and an exact sum that rounds
  once.
- Every change runs the whole test suite at 1, 2, 3, 4, 8 and 16 threads and
  requires the same bytes out of every program.

### The standard library

`std/mem`, `std/io`, `std/fmt`, `std/bytes`, `std/strings`, `std/unicode`,
`std/collections`, `std/json`, `std/crypto`, `std/num`, `std/rand`, `std/par`,
`std/os`, `std/fs`, `std/process`, `std/time`, `std/runtime`, `std/net` (TCP and
UDP, IPv4 and IPv6), `std/tls` (over OpenSSL, optional at build time),
`std/http` (HTTP/1.1 server and client, routing, keep-alive, streaming, static
files, timeouts and deadlines, HTTPS), `std/chan` and `std/testing` (tests and
benchmarks, run by `shield test`, in parallel by default).

### Tools

- `shield`, the compiler: `.sword` to LLVM IR, assembled and linked by `clang`.
- `swordls`, the language server, built on the same front end, so a diagnostic
  in the editor is the one the compiler gives.
- Editor support for VS Code, vim, Neovim and Emacs.

### Where it runs

macOS and Linux, on arm64 and x86-64, each change tested on both systems with
TLS and without. No Windows.

### What it does not do yet

No package manager. Use-after-free is not tracked: tasks cannot outlive what
they borrow, but freeing something and reading it is yours to avoid. Tens of
thousands of concurrent tasks, not millions. The HTTP client keeps one
connection per host rather than a pool.
