# Why Sword

Sword is for writing servers: thousands of connections, memory you decide about,
and a compiler that refuses programs where two tasks can touch the same thing at
the same time.

It is young. There is no package ecosystem, the standard library is what there is,
and nothing here has been through a decade of production. If that rules it out for
you, it rules it out — the rest of this page is about what the design actually buys,
and [what it does not](#what-you-give-up), so you can decide rather than guess.

## The one idea

Everything a task can reach is on the line that starts it, and a task cannot
outlive the block that started it.

```sword
scope {
    for off, part in dst.chunks(8) {
        spawn square(src[off..off+part.len], part)
    }
}
```

`spawn` takes a call and nothing else. `scope`'s closing brace is the join. Those
two rules together mean the compiler can see, at the `spawn`, every piece of memory
the new task will ever touch — so it can compare them. Two tasks writing the same
slice is a compile error. Eight tasks writing eight pieces of one array is fine,
because `chunks` hands out pieces that provably do not overlap.

That is the whole trade, and it is worth being clear about both halves:

**What it buys.** No data races, checked at compile time, with no ownership
annotations and no lifetimes to write. And because a task cannot outlive its
`scope`, it can borrow memory from the stack of the function that started it — the
example above lends two slices of a stack arena to eight tasks and allocates nothing
per task.

**What it costs.** No closures, so a callback that needs state is an interface
instead. No detached tasks: you cannot start work and walk away from it. Long-lived
shared state needs `shared[T]`, which is a lock, rather than fine-grained ownership.

And one limit worth knowing before you trust it: the checker compares accesses by
the binding they come from, so memory with no name behind it — a slice returned
straight out of a call and handed to a task without being bound to anything — is not
tracked. `atomic[T]` and `shared[T]` are the two deliberate exits, and both are safe
because there is no way to reach the value except through them.

Rust prevents the same class of bug with far more reach — across arbitrary
lifetimes, not just within a scope — and charges you a borrow checker for it. Go
does not prevent it at all: the race detector is a runtime tool, it only sees races
that actually happen while it is watching, and you have to remember to turn it on.
Sword takes most of the guarantee for a fraction of the machinery by giving up
open-ended task lifetimes. Whether that is a good deal depends entirely on whether
your tasks want to be open-ended. In a server, they usually do not: a connection
begins, is served, and ends.

## A task is an ordinary function

There is no `async`. A task is a function, and any function can wait:

```sword
func handle(c net.Conn) !void {
    mut buf := [1024]u8{}
    n := try c.Read(buf[..])   // the task stops here; the thread does not
    try c.Write(buf[0..n])
}
```

`handle` is callable directly and spawnable, and nothing in its type says which. In
Rust, `async fn` is a different kind of function: it returns a future, it can only
be awaited from another `async fn`, and the split runs all the way down through
every library you use — plus `Pin`, `Send`, `Sync` and a choice of executor. In
JavaScript and Python the same split exists under different names. Go does not have
it either, and this is one of the reasons Go is pleasant to write servers in.

The cost of avoiding it is that each task needs its own stack. Sword reserves a
megabyte per task and commits the pages a task actually touches, which measured on a
real server is about 33 KiB for an idle keep-alive connection. The ceiling that
follows is honest to state: each stack needs a guard page, the kernel counts a
protected range as a mapping, and Linux allows around sixty-five thousand of them —
so tens of thousands of concurrent tasks, not millions. An async runtime with
heap-allocated state machines goes further on the same memory. If you need a million
idle connections on one box, that is a real argument against this design.

Measured, on ten cores, with keep-alive:

| | |
|---|---|
| 4000 concurrent connections | 84 000 requests/second, 12 threads |
| 1000 concurrent connections | 146 000 requests/second, 34 MiB |
| 2000 idle connections | 65 MiB — 33 KiB each |
| 20 000 tasks, 16 KiB touched each | 338 MiB, 11 threads |

The waiting is uniform, which matters more than the throughput number. A socket
read, a sleep, a TLS handshake, a file read and a name lookup all put the *task*
down and leave the thread to somebody else. Files and name lookups cannot be
polled, so they go to a small pool of threads kept for exactly that: forty
concurrent file reads cost ten workers and ten pool threads, not forty threads.

## Nothing allocates behind your back

A function that touches the heap takes an allocator. There are no exceptions, not
even in the standard library:

```sword
func ReadAll(path string, mut a mem.Allocator) ![]u8
func TrimSpace(s string) string              // cannot allocate: nowhere to do it from
```

Reading a signature tells you whether it can allocate. That is not a style rule —
it is the only way to allocate, so it cannot drift. In practice it means an HTTP
handler gets a 16 KiB arena on its own task's stack, serves the request out of it,
and the arena goes away when the task does. No GC pauses to tune, no allocator to
instrument, no surprise in the middle of a hot path.

Go's allocation is invisible and its GC is the price — a very well-built price,
but one you cannot opt out of. C gives you `malloc` and no discipline about it.
Rust's `Vec` and `String` allocate from a global allocator by default, and
threading a custom one through a program is still awkward.

## Smaller things that add up

**A failure cannot be ignored.** `!T` cannot be discarded, and its value cannot be
reached until it is handled. Errors are declared with the sentence they say, cost
nothing to raise, and the same name from two packages is one error:

```sword
error Stale = "the cached answer is older than the caller allows"
```

**A mutex has the value inside it.** `shared[T]` can only be reached through
`lock`, so there is no way to touch the data without holding the lock — the same
idea as Rust's `Mutex<T>`, and the opposite of a `sync.Mutex` sitting next to the
field it is supposed to protect.

**A task cannot be forgotten.** A goroutine that nobody is waiting for is a leak
Go cannot see. A Sword task is joined by the closing brace of its `scope`, so that
shape does not exist: a task that never finishes blocks the join instead, which you
find out about immediately rather than through a memory graph three weeks later.

**The whole thing is small enough to read.** The compiler is about 10 000 lines of
C++, the runtime 3 300, the language server 1 300, and the standard library 4 900
lines of Sword with no hidden compiler primitives behind it. For a language you are
going to depend on, being able to read all of it in a weekend is worth something.

**The compiler is the language server.** `swordls` links the same lexer, parser and
checker as `shield` and calls the same `check()`. The colouring knows a type from a
function because it comes from the symbol table, not from a regular expression, and
a diagnostic in the editor is the diagnostic the compiler gives.

## Against each one in particular

**Go.** The same task model, and Go does it with a decade of polish, a real
ecosystem and the best tooling in the business. Sword differs in three ways that
matter: no garbage collector and no hidden allocation, races caught at compile time
rather than sometimes at run time, and tasks that cannot leak. If you want a
service written this afternoon with libraries for everything, write Go. If you want
to know where every byte went and have the compiler refuse your races, that is what
this is for.

**Rust.** Stronger guarantees than Sword gives — use-after-free, data races, across
any lifetime, not just inside a scope — and an ecosystem to match. The price is the
borrow checker and, for servers, the async split described above. Sword is a much
smaller language to hold in your head, and its concurrency reads like straight-line
code. If you need to prove more than "no races inside this scope", use Rust; it is
the better tool and it is not close.

**C.** Every platform, every library, and an ABI everything speaks — Sword links
against C for exactly that reason. What it adds on top: slices that know their
length and are checked, errors that cannot be ignored, no implicit allocation but
arenas and allocators as ordinary values, tasks with a scheduler, and a compiler
that refuses the concurrency bug you would otherwise find in production. What C
keeps is everything else.

## What you give up

Said plainly, because a page like this is worthless otherwise:

- **No ecosystem.** No package manager, no third-party libraries. The standard
  library is `std/mem`, `std/io`, `std/fmt`, `std/bytes`, `std/strings`,
  `std/collections`, `std/json`, `std/os`, `std/fs`, `std/time`, `std/runtime`,
  `std/net`, `std/tls`, `std/http`, `std/chan` and `std/testing`. That is the list.
- **Not memory-safe in Rust's sense.** Sword stops data races, stops a task from
  outliving the memory it borrowed, and refuses a function that hands back its own
  frame. It does not stop you from freeing something and reading it afterwards:
  that would take the ownership machinery this language deliberately does not have.
  Bounds and integer overflow are checked in the default build mode, `mem.Watch`
  and `mem.NewWatched` make released memory read as obviously wrong rather than
  plausible, and `shield test` turns the first of those on for you — but the
  discipline is yours. [The tour says what that discipline
  is](tour.md#memory-that-has-gone-away).
- **No closures.** Function values carry no captured state, by design — it is part
  of what makes `spawn` checkable. State goes in an interface.
- **Tens of thousands of tasks, not millions.** A stack each, and the kernel counts
  the mappings.
- **Young everywhere it counts.** TLS is a thin layer over OpenSSL rather than
  something audited. There is no Windows support. The HTTP client keeps one
  connection rather than a pool. No `io_uring`. No formal anything.
- **It is one person's language.** Decisions were made once, by one person, and
  some of them will turn out wrong.

## When not to use it

If you need a library that already exists, use the language that has it. If you need
guarantees beyond races and scopes, use Rust. If you need to run on a platform
nobody has tried, use C. If you are shipping something to customers next month, use
what you already know.

Use this if you are writing a server, you care where your memory goes, and you would
rather the compiler argue with you now than debug a race at three in the morning.
