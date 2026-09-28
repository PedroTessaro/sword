# Changes

Releases are numbered by [semantic versioning](https://semver.org): until 1.0,
a minor version may change the language, and each one says how. Each release is
also named for a step in making a blade, in the order they happen.

## 0.2.0 — Fold

*2026-09-28.* Folding the steel is how the flaws are worked out of it: this
release makes concurrency bugs you can reproduce, and numbers you can see.

### What changes for existing programs

- **`{}` prints a float as the shortest text that reads back as the same
  double.** It printed six places: `1e-9` came out as `0.0` and `1.0 / 3.0` as
  `0.333333`. Now `0.1` is `0.1`, `0.1 + 0.2` is `0.30000000000000004`, `1e-9`
  is `1e-9`, and whole numbers keep their `.0`. Plain from 1e-4 to 1e16, a
  mantissa and a power of ten outside.
- **`{.N}` is exact and always fixed point.** The digits are the double's own,
  rounded once with ties to even, as C's `printf` does: `{.20}` of `0.1` is
  `0.10000000000000000555`. A large number prints all its digits where it used
  to switch to exponent form.
- Negative zero prints with its sign.
- `std/json` reads numbers as the nearest double; `0.3` used to come out one
  unit high.

### Simulation

- **`shield test -sim`** runs each test on one thread with every scheduling
  choice drawn from a seed, once per seed (100 by default, `-seeds N`), and a
  failure comes with the seed and the command that makes it happen again
  (`-seed S`). A deadlock is reported as one instead of hanging. Time is
  virtual — it jumps to the next deadline when every task waits on it — and the
  wall clock starts at 2000-01-01 00:00:00 UTC.
- **`SWORD_SIM_SEED=n`** runs any program the same way. `--sim` also switches
  tasks at every atomic operation; without it the generated code is unchanged.
- Tests that open a socket are skipped under `-sim` until the network is
  simulated. A call out of the language gets a compiler warning under the
  simulator, which cannot replay it.

### Numbers

- **`strings.ParseF64`**: the double nearest the text, a tie going to the even
  one. What `{}` prints reads back as the same double.
- **`num.Sin` and `num.Cos` reduce ordinary arguments the way fdlibm does**:
  the same bits as before for every argument checked, and a sine of an argument
  in [-1000, 1000] went from about 174 ns to 11.7 ns on an Apple M4.

### Fixes

- A task can wait — for a timer, a socket, a `Wait` — inside a `lock`. On one
  thread that used to be reported as a deadlock that was not one, and a
  contended lock now puts the task down instead of spinning its thread.
- A task at the end of a `scope` parks instead of running the scope's tasks on
  its own stack, which could leave a task waiting for one buried beneath it.
- A socket made or accepted just after another was closed could read the old
  descriptor's generation and fail its first wait: `net.Dial` reported a failure
  over a connection that had been made. The TLS test lost one connection in a
  hundred runs on Linux this way.

### How changes get in

Every pull request now runs, on Linux and macOS: the whole suite with TLS and
without and at 1 to 16 threads, AddressSanitizer and ThreadSanitizer
(`tests/sanitize.sh`), every example program in the documentation
(`tests/docs.sh`), a check of commit subjects, a review of new dependencies,
and CodeQL. There are issue and pull request templates and a security policy.

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
