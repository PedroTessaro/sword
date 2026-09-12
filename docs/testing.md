# Testing

Tests live beside the code they test, in files ending `_test.sw`. `shield test`
builds them together with the package, runs them, and reports.

```sword
// std/strings/strings_test.sw
package strings

import "std/testing"

func TestTrimSpace(mut t *testing.T) !void {
    try t.Equal(TrimSpace("  ada  "), "ada")
    try t.Equal(TrimSpace("\t\nada\r\n"), "ada")
    try t.Equal(TrimSpace(""), "")
}
```

```sh
$ shield test std/strings
ok    6 tests  (0ms)
```

A test is a function whose name starts with `Test`, taking one `*testing.T` and
returning `!void`. Nothing registers it: the compiler finds it and writes the
entry point that runs it.

The test file says `package strings`, so it sees the package's own names —
`TrimSpace`, not `strings.TrimSpace`. It is inside the package, which also means
it can test what the package does not export.

**A `_test.sw` file is left out of every ordinary build.** That is what lets the
tests sit next to the code: a program that imports `std/strings` does not carry
them, and cannot see them.

## Failing

Two shapes, and the difference is whether the test goes on:

| | keeps going | ends the test |
|---|---|---|
| a value | `try t.Same(got, want)` | `try t.Equal(got, want)` |
| a condition | `try t.Check(ok)` | `try t.Require(ok)` |
| a message | `try t.Failf(...)` | `try t.Fatalf(...)` |

"Ends the test" is not a special power. `Equal`, `Require` and `Fatalf` return an
error, and the `try` in front of them carries it out of the test function — the
same way `try` leaves any other function early. There is no unwinding here and
nothing to remember: if you wrote `try`, a failure stops.

Everything takes `try`, including the ones that keep going, because all of them
write output.

```sword
func TestParse(mut t *testing.T) !void {
    doc := try Parse([]u8(src), t.Mem)   // a failure here ends the test too
    root := doc.Root()

    try t.Equal(doc.Kind(root), Kind.Object)
    try t.Same(doc.Len(root), 5)         // wrong count, but carry on
    try t.Same(doc.GetText(root, "name"), "ada")
}
```

A test that returns an error it did not mean to — the `try Parse` above — is
reported as a failure with the error's code.

`t.Equal` compares through `any`, so it takes integers, floats, bools, strings
and enums, and prints both sides when they differ. Strings come out quoted,
because a failure about whitespace is unreadable otherwise:

```
--- FAIL  TestTrimSpace
      got "\ta", want "a"
```

Anything `any` cannot carry — a struct, a slice — you compare yourself and report
with `t.Failf`.

## Memory

`t.Mem` is an allocator the test can use, reset before each test runs. So a test
allocates and never frees:

```sword
func TestListGrows(mut t *testing.T) !void {
    mut xs := try NewList[i64](t.Mem, 2)
    for i in 0..100 {
        try xs.Push(i64(i) * 2)
    }
    try t.Equal(xs.Len(), 100)
}
```

It is an arena of 256 KiB. A test that needs more builds its own, the same way
any other Sword code would.

## Subtests

`t.Run` gives a section of a test its own label, which is what shows up when it
fails:

```
--- FAIL  TestParseU64/12a
      accepted it
```

There are no closures, so a subtest body cannot capture anything. `t.RunWith`
hands it one value through `t.Arg`:

```sword
func TestParseU64(mut t *testing.T) !void {
    bad := [4]string{"", "12a", "-1", " 7"}
    for s in bad {
        try t.RunWith(s, rejects, s)
    }
}

func rejects(mut t *testing.T) !void {
    ParseU64(t.Arg.Text) catch {
        return
    }
    try t.Failf("accepted it")
}
```

A case with more than one value in it does not fit in `t.Arg`. Then keep the loop
in the test and name the case in the message, which costs one line and reads just
as well:

```sword
struct trimCase {
    input string
    want  string
}

func TestTrimSpaceTable(mut t *testing.T) !void {
    cases := [2]trimCase{
        trimCase{input: "  ada  ", want: "ada"},
        trimCase{input: "   ", want: ""},
    }
    for c in cases {
        got := TrimSpace(c.input)
        if got != c.want {
            try t.Failf("{}: got {} want {}", c.input, got, c.want)
        }
    }
}
```

## Skipping

```sword
try t.Skip("needs a network")
```

Ends the test without failing it, and says why on the way out. A skip nobody can
see is a test nobody ever comes back to.

## Running them

```sh
shield test std/strings          # one package
shield test ./mypkg              # any directory
shield test foo_test.sw          # a single file
shield test std/json -o /tmp/jt  # keep the binary
```

The exit status is zero only when nothing failed, so it drops into a build
without ceremony. `make test` in this repository runs the compiler's own tests,
the language server's, and then every `std/*/*_test.sw` package.

Testing a program rather than a library works too: its `main` is set aside for
the run, not called.

## What is missing

**Benchmarks.** There is no `testing.B`. Measuring is easy enough by hand with
`std/time`, but there is no harness that repeats a body and reports per
operation.

**Parallel tests.** Everything runs in order. Each test would need its own
allocator and its own result, and `spawn` takes a direct call rather than a
function value, so this needs work on the language and not only on the library.

**Line numbers.** A failure names the test and the subtest, not the file and
line: there is no way to ask for the caller's position. Subtest labels are the
substitute, and they are usually enough.

**Error names.** A test that returns an unexpected error reports its code, not
its name. Errors are codes at run time and nothing maps them back.
