# Concurrency

Most languages give you threads and trust you. Sword gives you tasks and checks
you: a program where two tasks can touch the same memory at the same time does
not compile. This chapter is about how that works and what it costs you in
exchange.

Read [the tour](tour.md) first if you have not.

## Tasks and scopes

```sword
import "std/mem"

func shade(mut row []i64, gain i64) {
    for i in 0..row.len {
        row[i] *= gain
    }
}

func main() !int {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])
    mut rows := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    for i in 0..rows.len {
        rows[i] = 1
    }

    scope {
        for part in rows.chunks(8) {
            spawn shade(part, 3)
        }
    }

    return int(rows[0] + rows[63])
}
```

`spawn` only exists inside `scope`, and the closing brace of the `scope` is the
join — it waits for every task it started. There is no handle to keep, nothing
to remember to wait on, and no way for a task to outlive the block that created
it.

That last part is the point. Because a task cannot survive its scope, the
compiler knows a slice lent to one stays valid for the task's whole life. It is
why you can hand out pieces of memory that live on the parent's stack, or in an
arena the parent will throw away right after.

### `spawn` takes a call, and only a call

```sword
spawn shade(part, 3)
```

Not a block, not a closure. The arguments are evaluated in the parent and
copied into the task.

This is a real restriction and it is deliberate. Everything a task can reach is
written on that one line, which is exactly what the race checker needs to look
at. A closure would hide the same information inside a body you would have to
read.

### A task returns nothing

There is no `Task[T]`, no handle, no `await`. A task writes into memory the
parent gave it:

```sword
scope {
    for off, part in results.chunks(16) {
        spawn compute(input[off..off+part.len], part)
    }
}
// results is complete here
```

That sounds limiting until you notice it removes a whole category of
synchronisation. The parent allocated the destination before spreading the
work, each task owns a disjoint piece of it, and the join is the only handoff.

Errors do still come back. A task running a function that returns `!void`
carries its failure out to the scope:

```sword
func check(xs []i64, limit i64) !void {
    for i in 0..xs.len {
        if xs[i] > limit {
            return error.TooBig
        }
    }
}

func validate(data []i64) !void {
    scope {
        for part in data.chunks(32) {
            spawn check(part, 1000)
        }
    }
}
```

If any task fails, `validate` fails. The scope waits for everybody first and
propagates the first error it saw.

### Nothing is cancelled

A failing task does not stop its siblings. They run to completion and then the
scope reports the error.

That is a choice, not an oversight. Cancellation means interrupting a task part
way through, possibly while it is in the middle of writing into memory borrowed
from the parent's arena — and making that safe is a much larger problem than it
looks. Sword would rather waste some work than hand you a half-written buffer.

### You cannot leave a scope early

```sword
scope {
    spawn work(a)
    if done {
        return 1      // compile error
    }
}
```

No `return`, no `break`, no `try` that escapes the block. The tasks are still
running on that stack frame; leaving would pull the floor out from under them.

In practice this means doing the fallible work before the scope rather than
inside it. It is the restriction you will bump into most, and it is the price
of the join being a brace rather than something you have to remember.

## The race checker

One rule covers it:

> Within a `scope`, two accesses to the same memory are an error unless both
> only read, or they are pieces of one partition.

```sword
scope {
    spawn read(xs)
    spawn read(xs)      // fine: nobody writes
}

scope {
    spawn write(xs)
    spawn read(xs)      // error: one writes while the other reads
}
```

Whether a task reads or writes comes from the parameter it is passed to. `mut`
on a parameter means the function may write through it; without `mut`, it
cannot. So the signature the task was declared with is what the checker reads,
and you do not annotate anything extra.

### The parent counts too

```sword
scope {
    spawn read(xs)
    xs[0] = 9           // error
}
```

The join is the closing brace, so whatever the parent does inside the block
happens alongside the tasks. It is easy to forget that the parent is a
participant; the checker does not.

### What is actually shared

Copying a value is only safe when the copy owns everything it reaches.

| | |
|---|---|
| Scalars, `struct` of scalars, `[N]T`, `string` | copied outright, never tracked |
| `[]T` | the two-word header copies; the elements stay behind |
| `*T`, `[*]T`, an interface value | the pointee stays behind |
| A struct containing any of the above | tracked, through the field |

So this is caught, even though `Job` is passed by value:

```sword
struct Job {
    data []i64
}

func run(mut j Job) {
    j.data[0] = 1
}

// two tasks writing the same elements, through a struct
scope {
    spawn run(j)
    spawn run(j)        // error
}
```

## Partitions

Handing different pieces of one slice to different tasks is safe, and it is the
central pattern of data parallelism. Sword does not try to prove your indices
do not overlap. It gives you the one construction that cannot produce
overlapping pieces:

```sword
for part in dst.chunks(8) {
    spawn fill(part)
}
```

Each `part` is a distinct piece, so they can all be written in parallel. The
last one is short when the length does not divide evenly. (`for x in xs` walks
the elements one at a time, which is the loop to reach for when there is no task
involved.)

Slicing by hand inside a loop is rejected:

```
error: every turn of this loop hands the same memory to a new task, so the
       tasks would overlap in 'dst'
note: walk it with 'for part in dst.chunks(n)' to give each task a piece of
      its own
```

The alternative would be inferring disjointness from a syntactic pattern like
`xs[i*k .. i*k+n]`. That works until you write the same thing slightly
differently, and then it rejects correct code for reasons that are hard to
explain. A guarantee by construction never fails quietly.

When you need the position as well as the piece — to index a second array in
step — bind both:

```sword
scope {
    for off, part in dst.chunks(8) {
        spawn square(src[off..off+part.len], part)
    }
}
```

`src` is read by every task, which is allowed. `part` is written by exactly
one.

## `parallel for`

For the common case of "do this to every element", there is a loop that spreads
itself:

```sword
parallel for i in 0..n {
    xs[i] = xs[i] * 2
}
```

This is the one construct in the language that captures. The compiler works out
which locals the body uses, hands the workers a block of pointers to them, and
that is safe because the loop is synchronous — the parent's frame is not going
anywhere while it runs.

Because iterations run on different workers, the body may only write what
exactly one of them can reach: `xs[i]`, with `i` the loop variable. And once
the loop writes a name, every other mention of it has to be indexed too:

```sword
parallel for i in 0..n {
    xs[i] = xs[i+1]     // error: reads what another iteration writes
}
```

That is the neighbour bug, and it is the kind of thing that shows up in
production as a heisenbug three months later.

### Reductions

Accumulating into one variable is the exception that needs help:

```sword
mut total i64 = 0
parallel for i in 0..n reduce(+: total) {
    total += xs[i]
}

mut biggest i64 = 0
parallel for i in 0..n reduce(max: biggest) {
    if xs[i] > biggest {
        biggest = xs[i]
    }
}
```

Each worker gets a private copy, started at the operator's identity, and they
are folded together once at the end — so there is no contention per iteration.
Without the `reduce` clause, writing the variable in the body is an error, and
the message tells you to add it.

The body does the per-element combining; the clause only says how the workers'
copies are joined. `+`, `&`, `|`, `min` and `max` are available, and `+` also
applies to floats.

### How much faster

A loop with real work in the body, 8192 iterations, on a ten-core machine:

| Threads | Time | Speedup |
|---|---|---|
| 1 | 0.507 s | 1.00× |
| 2 | 0.264 s | 1.92× |
| 4 | 0.156 s | 3.25× |
| 8 | 0.100 s | 5.07× |

`SWORD_THREADS` overrides the worker count, which defaults to one per core.
Useful for measuring, and for pinning down whether a bug is a race.

## The scheduler

The runtime is a few hundred lines and links as a static archive, so a program
that never spawns anything carries none of it.

Each worker has its own deque. The owner pushes and pops at one end, thieves
take from the other, and the victim rotates so workers do not all converge on
the same target. `parallel for` cuts the range into several chunks per worker —
enough slack for stealing to even out an uneven body, without paying task
overhead per iteration.

A thread waiting on a scope does not idle. It runs whatever work it can find
until the count reaches zero, which is what makes nested scopes safe from
deadlock: the thread blocked on the inner scope is also a thread available to
run its tasks.

Task arguments up to 96 bytes ride inside the task itself, and finished tasks
go back on a per-worker free list, so spawning in a loop does not touch the
allocator. Stacks come from the same kind of pile: mapping one costs far more
than keeping it.

A poller thread sits in kqueue or epoll and does nothing else. It owns no work;
it moves tasks the kernel has declared ready back onto the run queues.

## Waiting

**A task has its own stack.** That is what lets it stop in the middle of
something and be picked up later, and it is the whole reason a socket does not
cost a thread.

When a task reads from a socket with nothing to say, the runtime puts the task
down: it registers the descriptor with a poller — kqueue or epoll — switches
back to the worker's own stack, and the worker goes to whoever does have data.
When the kernel says the descriptor is ready, the task goes back on a run queue
and whichever worker gets to it first resumes it, on the line after the read.

Nothing in your program says any of this. `try c.Read(buf)` is the whole of it:

```sword
func handle(c net.Conn) !void {
    mut buf := [1024]u8{}
    n := try c.Read(buf[..])   // the task stops here; the thread does not
    try c.Write(buf[0..n])
}
```

A stack is reserved rather than committed, so what a waiting task actually costs
is the few pages it has touched. Measured on a real server: a keep-alive
connection sitting idle costs about 49 KiB, against roughly 80 KiB and a whole
thread before. Three thousand of them run on twelve threads.

An overflow hits a guard page and faults where it happened, rather than writing
quietly through somebody else's stack.

### What still holds a thread

Not everything can be put down. Name resolution is one call in the C library
with no way in or out of the middle, and so is sleeping in some cases. Those say
so first:

```
sword_blocking_enter();
getaddrinfo(...);
sword_blocking_exit();
```

A parked thread stops counting as scheduler capacity, so the pool hires a
replacement while it is gone and retires it after 200 ms of finding no work. It
is the fallback now rather than the main mechanism, but it is still what keeps
one slow name lookup from stalling everything.

Code that is not in a task at all — the main function, before any `scope` — has
nothing to put down, so it waits on the thread the ordinary way.

Three numbers control the shape of it:

| Variable | Meaning |
|---|---|
| `SWORD_THREADS` | Workers. Defaults to one per core. |
| `SWORD_MAX_THREADS` | How large the pool may grow to cover threads stuck in the kernel. Defaults to 512. |

The ceiling is a brake rather than a wall. A pool at its limit with every thread
parked and work still queued would be a program that has stopped, so in that one
case the runtime goes over the limit instead.

### Deadlines

A timeout bounds one wait. It cannot bound a client that sends a byte every
twenty milliseconds forever, because no single wait ever runs out. That is what
a deadline is for:

```sword
try c.SetTimeout(time.Seconds(5))                      // any one wait
try c.SetDeadline(time.Now().Add(time.Seconds(30)))    // the whole exchange
```

Past the deadline every read and write on that connection answers
`error.Timeout`, and the handler leaves through the `try` it was already
written with. Nothing is interrupted: a deadline is collected where the task was
going to stop anyway, which is the only place it can be collected without
tearing a task in half.

## Shared counters

`reduce` covers accumulating over a loop. For anything else — a hit counter, a
flag, a sequence number — there is `atomic[T]`:

```sword
mut hits := atomic[u64](0)

scope {
    for part in rows.chunks(64) {
        spawn count(part, &hits)
    }
}
total := hits.Load()
```

An atomic is the one exemption the race checker makes: several tasks may write
one at the same time. Everything reached through it goes through `Load`,
`Store`, `Add`, `Sub`, `And`, `Or`, `Swap` and `CompareSwap`, each a single
instruction with sequentially consistent ordering. There is no way to reach the
raw value, which is what makes the exemption safe to grant.

A plain integer gets no such treatment:

```sword
mut counter u64 = 0
scope {
    spawn bump(&counter)
    spawn bump(&counter)   // error: two tasks writing 'counter'
}
```

## Shared structures

An atomic covers one scalar. A hit table, a work queue, a cache — anything with
more than one field in it — needs something else, because a counter's worth of
synchronisation cannot cover a structure whose invariants span several writes.

That is `shared[T]`:

```sword
import "std/collections"
import "std/mem"

func count(mut table *shared[collections.Map[u64]], words []string) !void {
    for i in 0..words.len {
        lock m := table {
            seen := m.Get(words[i]) orelse 0
            try m.Set(words[i], seen + 1)
        }
    }
}

func main() !int {
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])

    mut table := shared[collections.Map[u64]](
        try collections.NewMap[u64](&arena, 64))

    mut words := [2]string{}
    words[0] = "ada"
    words[1] = "grace"

    scope {
        for i in 0..6 {
            spawn count(&table, words[..])
        }
    }

    lock m := &table {
        return int(m.Len())
    }
}
```

Six tasks writing one map, and it compiles. The exemption the race checker makes
is the same one it makes for an atomic, and it rests on the same thing: **there
is no way to reach the value except through the lock.** `shared[T](v)` builds
one, `lock` opens one, and the type has no fields you can read.

### The block is the critical section

```sword
lock m := table {
    ...
}
```

The opening brace takes the mutex and the closing brace releases it. So does a
`return` out of the middle, a `break`, a `continue`, or a `try` that fails —
it unwinds the same way `defer` does, and for the same reason. There is no
`Unlock` to forget and no way to unbalance the pair.

Inside, the name is a mutable `*T`. Outside, it does not exist.

Nothing here is marked `mut`, and that is on purpose. The type already says the
value will be written by whoever holds the lock, so the binding's mutability has
nothing left to decide — exactly as with an atomic. A shared can also sit in a
struct field, and the struct around it stays an ordinary struct:

```sword
struct Server {
    name  string
    tally shared[Tally]
}

func serve(s *Server) {
    lock c := &s.tally {
        c.hits += 1
    }
}
```

### Two rules

**A lock inside a lock on the same value is an error.**

```sword
lock a := &table {
    lock b := &table {      // error: 'table' is already locked here
    }
}
```

That is the version the compiler can see. The version it cannot — a second
`lock` reached through a function call — aborts at runtime with a message rather
than hanging, because the guard remembers which thread holds it.

**A task cannot outlive the lock it starts under.**

```sword
scope {
    lock c := &table {
        spawn touch(c)      // error: this task would outlive the 'lock'
    }
}
```

The lock releases at its own brace, the scope joins at its own, and the task is
still running in between. Put the `scope` inside the `lock` — which is safe and
useful, parallel work over a locked structure — or put the `lock` inside the
task.

### What it costs

A contended lock spins for a moment and then sleeps, and while it sleeps it
tells the scheduler, so a thread waiting behind a long critical section does not
cost a core. Eight tasks doing 200 000 locked increments each — 1.6 million
lock/unlock pairs — take about 25 ms on a ten-core machine. The same work
through one `atomic[u64]` takes about 45 ms, because eight threads hammering one
cache line contend harder than eight threads taking turns. Read nothing general
into that: the point is only that the lock is not going to be your bottleneck.

What is missing: no read-only lock, so two readers still take turns, and no
try-lock, so there is no way to do something else instead of waiting.

## What is not here yet

**Channels.** The syntax is reserved and the design is settled, but nothing is
implemented.

**Memory with no name behind it.** The checker compares accesses by the binding
they come from. A slice returned straight out of a call, never bound to
anything, is not tracked — there is nothing to compare it against.

**Growable stacks.** A task's stack is reserved at a megabyte and the pages it
touches are what it costs, which is fine into the low tens of thousands of
tasks. Past that the reservations themselves start to matter: Linux counts
mappings, and the usual limit is around sixty-five thousand of them, or about
thirty thousand tasks. Go grows and moves stacks instead, which needs the
compiler's help to find and rewrite the pointers into them.
