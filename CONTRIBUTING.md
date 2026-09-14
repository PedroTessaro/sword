# Contributing to Sword

Sword is young, and there is room to help in every part of it: the compiler, the
runtime, the standard library, the documentation and the editors. This page says
how a change gets in.

## Where to start

**Found a bug?** Open an issue with the smallest program that shows it, what you
expected, what happened, and where you ran it: the commit you built
(`git rev-parse --short HEAD`), the operating system and the processor. A program
the compiler accepted and should have refused counts as a bug as much as a crash
does, and for the race checker it is the more important kind.

**Want to change the language?** Syntax, semantics, or what the checker accepts
starts as an issue, not a pull request. Describe the problem with code: what you
have to write today, and what you would write instead. The design decisions are
the maintainer's, and a change to the language is merged only once its issue has
been agreed on — that conversation is cheaper than a pull request nobody can
take. Some things are settled, because they are why the language exists: nothing
allocates implicitly, a task's lifetime is a lexical scope, a binding is
immutable unless it says `mut`, and the compiler refuses a program in which two
tasks can touch the same memory at once. [Why Sword](docs/why.md) explains what
they buy.

**Everything else** — a fix, a function the standard library is missing, a
clearer page of documentation, support for another editor — can come straight
as a pull request. For anything large, an issue first saves work on both sides.

## Building and testing

[docs/install.md](docs/install.md) lists what the build needs. Then:

```sh
make
make test
```

`make test` has to pass. It runs the compiler's tests in `tests/`, the language
server's, the standard library's, and the editors' indentation for whichever of
vim, Neovim and Emacs you have installed.

A fix comes with a test that fails without it. A compiler test is a file in
`tests/` whose first comment says what should happen:

```sword
// expect: 0
// expect-output: hello
import "std/io"

func main() !int {
    try io.Println("hello")
    return 0
}
```

`// expect-error: <message>` instead says the program must not compile, and with
what error. A change to the standard library gets a test in the package's own
`_test.sword` file; `shield test std/<package>` runs it.

A change to the runtime, or to anything concurrent, needs more than one run.
Build with sanitizers as in
[the install guide](docs/install.md#building-the-compiler-with-sanitizers), and
run the concurrency tests many times with `SWORD_THREADS` at 1, 2, 4, 8 and 16.
A test that fails one time in twenty is a bug until shown otherwise: the run
that failed is usually the one telling the truth. Say in the pull request what
you ran.

The VS Code extension has its own test, which downloads a copy of VS Code the
first time:

```sh
cd editors/vscode
npm install
npm test
```

## Writing the change

Match the code around it: its names, how much it comments, how it is laid out.
A comment says why, not what the next line does. Code, comments and
documentation are in English.

The language's rules apply to its own library. A function in `std` that touches
the heap takes an `Allocator`, like everyone else's.

Documentation is part of the change. A new feature, flag or library function
updates the page that describes it — the [reference](docs/reference.md), the
[tour](docs/tour.md) or the guide for that area — written for somebody learning
the language.

## Commits

Each commit is one coherent change, and each builds and passes the tests on its
own. Messages follow the conventional form — `feat(scope):`, `fix(scope):`,
`docs:`, `test:`, `perf:`, `refactor:`, `chore:` — with a body that says why the
change was made and what it cost. A number in a commit message or in the
documentation is one you measured; say how.

## Where things are

| | |
|---|---|
| `src/` | the compiler, `shield`: lexer, parser, checker, lowering, LLVM output |
| `lsp/` | the language server, `swordls`, sharing the compiler's front end |
| `rt/` | the runtime: scheduler, tasks, the poller, networking, TLS |
| `std/` | the standard library, written in Sword |
| `tests/` | the compiler's tests, and the scripts that run every suite |
| `editors/` | vim, Neovim, Emacs and VS Code |
| `docs/` | the documentation |

## License and conduct

Sword is under the [MIT License](LICENSE), and so is what you contribute to it:
by sending a pull request you agree that your change is released under the same
terms.

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
