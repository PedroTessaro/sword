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
last one is short when the length does not divide evenly.

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
allocator.

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

## What is not here yet

**`shared[T]`.** An atomic covers scalars. There is still no mutex-protected
wrapper for a whole structure — a map, a queue, a cache — so sharing one of
those for writing remains a compile error.

**Channels.** The syntax is reserved and the design is settled, but nothing is
implemented.

**Memory with no name behind it.** The checker compares accesses by the binding
they come from. A slice returned straight out of a call, never bound to
anything, is not tracked — there is nothing to compare it against.

**Async I/O.** I/O is synchronous. A task blocked on a socket holds its worker.
Making tasks suspend on I/O the way goroutines do would need segmented stacks
or a state-machine transform in the compiler, and that is a much bigger project
than the rest of the scheduler put together.
