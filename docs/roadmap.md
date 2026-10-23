# Roadmap

Where Sword is going, in the order it is likely to get there. Versions stay
below 1.0 until the language stops changing under people's feet, and each one
is named for a step in making a blade: Tamahagane and Fold are out, Quench is
next, and the last is Edge, at 1.0.

Nothing here has a date. A version ships when what it promises works and has
been measured, and the [changelog](../CHANGELOG.md) says what each one actually
did. A minor version may still change the language before 1.0, and when it
does, its changelog entry says how.

## What does not move

Some things are not on the roadmap because they are not going to change. They
are why the language exists:

- nothing allocates without an `Allocator` being passed in;
- a task lives inside the `scope` that started it, and the closing brace is the
  join;
- bindings are immutable unless they say `mut`;
- a program with a data race does not compile;
- there are no closures, and `spawn` takes a call and nothing else, so that
  everything a task can reach is written on one line.

A proposal that needs one of these to bend is a different language.

## 0.3 Quench — packages

Until now a program could use the standard library and code sitting next to it,
and nothing else. Quench adds packages, fetched from git:

```
module github.com/pedro/site
sword 0.3

require github.com/ana/sqlite v1.4.0
```

`shield get` adds a dependency; a build fetches whatever `sword.mod` asks for
and the cache does not have yet. Versions are git tags, chosen by minimal version
selection: the newest version anybody asked for, never the newest that exists.
`sword.sum` holds a hash of each version's contents, so a tag moved to other code
is caught. `shield vendor` copies everything into the project for building
without a network, and `replace` points a dependency at a local directory while
both are being worked on. There is no central registry, and fetching a package
never runs any of its code: a Sword package has no build script.

A package is compiled with the program, from source, the way the standard
library already is. That is what lets the race checker and `det` see through
it.

Quench also changes some things that compiled under Fold. These are already in
`develop`:

- a copy of a slice or pointer lent read-only cannot be written through, and the
  race checker sees two names for the same memory as the same memory;
- `lock read` for any number of readers at once, and `if lock` for not waiting;
- `~x` is the complement, and a statement on its own has to call, send or
  receive;
- tests check with `t.Equal` and carry on, `try t.Must` to stop, and neither the
  JSON writer nor an HTTP response asks for `try` at every write.

Still to come in Quench: `extern blocking func`, for a C call that blocks
without holding a scheduler thread; `shield new` and `shield doc`; and builds
that check independent packages in parallel and keep the standard library from
being redone every time.

## 0.4 Temper — speed

Sword gets correct code out of LLVM, but not yet fast code. The compiler emits
bounds checks it could prove away, calls everything with the C convention, and
never lets LLVM see into the runtime. Temper is about that:

- drop the bounds checks the language already proves, such as `xs[i]` inside
  `for i in 0..xs.len` over a slice that cannot change;
- `noalias` where the race checker has proved two pieces disjoint — only there,
  since a wrong one is undefined behaviour;
- a faster calling convention for functions nothing outside the program calls,
  and link-time optimisation with the runtime's hot paths;
- smaller binaries.

Every change is measured before and after, and the
numbers go out together with a comparison against C, C++, Rust and Go: the same
algorithms, idiomatic in each language, with versions and flags written down.

## 0.5 Polish — smaller machines

Sword assumes a 64-bit computer with an operating system. Polish takes that
apart:

- embedded Linux on 64-bit ARM, tested on real boards;
- `--target` for 32-bit and 16-bit machines, with layouts worked out for the
  target's pointer size instead of assumed;
- a standard library that works without an operating system. Which functions
  need one is inferred, the way `det` is, and using one on a bare-metal target
  is an error that says which call reached the system;
- a runtime for microcontrollers: tasks with small fixed stacks, cooperative,
  and interrupts.

## 0.6 — software that must not fail

What the compiler already refuses makes Sword a reasonable fit for software where
a failure costs more than a restart. 0.6 adds the guarantees that kind of work
asks for:

- a mode where nothing allocates once the program has started;
- the worst-case stack of every task, worked out by the compiler, with
  unbounded recursion refused;
- functions proved not to panic: no bounds check, overflow or `unreachable` on
  any path through them;
- a stricter subset in the spirit of MISRA, for code that has to follow one.

Certification itself is a process, not a compiler, and stays out of scope.

## Not yet placed

Wanted, and not yet given a version:

- tagged unions with an exhaustive `switch` over them;
- generic methods;
- evaluating `const X = f(...)` at compile time when `f` is `det`;
- debug information and a backtrace on panic;
- simulated networking for `shield test -sim`;
- `std/term`, and a terminal UI framework built on it as a package of its own;
- more of C: structs passed by value, unions, `packed` and `align`, generating
  bindings from a header, and calling Sword from C;
- `mut` on an interface method's receiver, so that a call through an interface
  is checked like any other ([#78](https://github.com/swordlang/sword/issues/78)).

## 1.0 Edge — Sword in Sword

The compiler is written in C++ today. Before 1.0 it is rewritten in Sword, one
stage at a time, and each stage is checked against the C++ one on everything it
can print — the syntax tree, Sword's own IR, the LLVM IR — before the next is
started. Edge is the version whose compiler builds itself, twice, and gets the
same binary both times. The C++ compiler stays as the seed for building it on a
new machine.

## Having a say

The order above is a plan and not a promise. If something here matters to what
you are building, or something missing does, open an issue that says what you
would write today and what you would rather write. That is how
cross-compilation and bare-metal targets got onto this page.
