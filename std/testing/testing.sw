package testing

import "std/bytes"
import "std/fmt"
import "std/io"
import "std/mem"
import "std/os"
import "std/strings"
import "std/time"

// Memory each test gets. Tests run at the same time by default, so this is per
// task rather than shared, and a test that needs more builds its own arena.
const ArenaSize = 262144
// How many tests may run at once. Sword has no global state, so two tests
// cannot reach each other except through a port, a file or the clock — the
// reason other languages default to running them in order simply is not here.
const DefaultParallel = 64
// How deep `Run` may nest. Deeper than this and the label stops being a label.
const MaxDepth = 4

// What `shield test` hands the runner: a name and the function to call. Written
// out by the compiler, one per `Test*` function it found.
struct Case {
    Name string
    Run  func(mut *T) !void
}

// A function type carries types and not names, so the field above reads
// `func(mut *T) !void`: a test takes one test, and may fail.
//
// A package-qualified struct literal is not a thing yet either, so the generated
// main says `testing.NewCase(...)` rather than building a Case by hand.
func NewCase(name string, run func(mut *T) !void) Case {
    return Case{Name: name, Run: run}
}

// Everything one test needs. It is handed in rather than reached for, which is
// what lets the runner reset the memory and collect the result.
struct T {
    // The test's own memory. Each test has its own, so a test never frees.
    Mem mem.Allocator
    // What `RunWith` was given. There are no closures, so this is how a subtest
    // is told which case it is: read `Arg.Text` or `Arg.Int`. It carries
    // nothing unless RunWith put something there.
    Arg any

    name   string
    subs   [MaxDepth]string
    depth  u64
    failed bool
    // Everything this test said, kept until it finishes. Tests run together, so
    // writing straight to the terminal would interleave them line by line.
    out bytes.Buffer
}

func nothing() any {
    return any{Kind: 0, Int: 0, Real: 0.0, Text: ""}
}

func newT(name string, mut a mem.Allocator) !T {
    return T{Mem: a, Arg: nothing(), name: name, subs: [MaxDepth]string{},
             depth: 0, failed: false, out: try bytes.New(a, 512)}
}

func (t *T) Name() string {
    return t.name
}

func (t *T) Failed() bool {
    return t.failed
}

// `--- FAIL  TestName/sub`, written once per message. Repeating it is what keeps
// a run readable when several things go wrong in one test.
func (mut t *T) header() !void {
    t.failed = true
    try t.out.WriteString("--- FAIL  ")
    try t.out.WriteString(t.name)
    for i in 0..t.depth {
        try t.out.WriteByte(47) // '/'
        try t.out.WriteString(t.subs[i])
    }
    try t.out.WriteString("\n")
}

// Marks the test failed and says nothing. Use it where the reason is already on
// screen.
func (mut t *T) Fail() !void {
    try t.header()
}

func (mut t *T) Failf(format string, args ...any) !void {
    try t.header()
    try t.out.WriteString("      ")
    try fmt.Format(&t.out, format, args)
    try t.out.WriteString("\n")
}

// Same, and ends the test: the error travels out through the `try` at the call
// site, which is the only way to leave a function early here.
func (mut t *T) Fatalf(format string, args ...any) !void {
    try t.Failf(format, args...)
    return error.Failed
}

func (mut t *T) Logf(format string, args ...any) !void {
    try t.out.WriteString("      ")
    try fmt.Format(&t.out, format, args)
    try t.out.WriteString("\n")
}

func (mut t *T) Check(ok bool) !void {
    if !ok {
        try t.Failf("check failed")
    }
}

func (mut t *T) Require(ok bool) !void {
    if !ok {
        try t.Fatalf("requirement failed")
    }
}

// Two values are the same when they carry the same thing. An integer and an
// unsigned integer compare by value, because `5` and `u64(5)` are the same
// number however they were written.
func alike(a any, b any) bool {
    counted := a.Kind == fmt.KindInt || a.Kind == fmt.KindUint
    if counted && (b.Kind == fmt.KindInt || b.Kind == fmt.KindUint) {
        return a.Int == b.Int
    }
    if a.Kind != b.Kind {
        return false
    }
    switch a.Kind {
    case fmt.KindBool, fmt.KindPointer:
        return a.Int == b.Int
    case fmt.KindFloat:
        return a.Real == b.Real
    case fmt.KindString:
        return a.Text == b.Text
    default:
        return true // two of nothing
    }
}

// Strings are quoted so that trailing space and tabs are visible, which is
// exactly what one of these failures is usually about.
func (mut t *T) show(label string, v any) !void {
    try t.out.WriteString(label)
    if v.Kind == fmt.KindString {
        try fmt.Quote(&t.out, v.Text)
    } else {
        try fmt.Value(&t.out, v)
    }
}

func (mut t *T) mismatch(got any, want any) !void {
    try t.header()
    try t.out.WriteString("      ")
    try t.show("got ", got)
    try t.show(", want ", want)
    try t.out.WriteString("\n")
}

// Records a mismatch and carries on, for when the rest of the test still has
// something to say.
func (mut t *T) Same(got any, want any) !void {
    if !alike(got, want) {
        try t.mismatch(got, want)
    }
}

func (mut t *T) Equal(got any, want any) !void {
    if !alike(got, want) {
        try t.mismatch(got, want)
        return error.Failed
    }
}

// Ends the test without failing it. The reason is printed, because a silently
// skipped test is one nobody ever comes back to.
func (mut t *T) Skip(why string) !void {
    try t.out.WriteString("--- SKIP  ")
    try t.out.WriteString(t.name)
    try t.out.WriteString(": ")
    try t.out.WriteString(why)
    try t.out.WriteString("\n")
    return error.Skipped
}

// A test within a test, which is what gives a section of one its own label. A
// subtest that fails fails the test around it, and one that ends early does not
// end that one.
func (mut t *T) Run(name string, body func(mut *T) !void) !void {
    try t.RunWith(name, body, nothing())
}

// The same, with one value handed to the body through `t.Arg`. A subtest body is
// a plain function and there are no closures, so this is the only way it can be
// told which case it is. For a case with more than one value in it, loop over
// the table in the test itself and name the case in the message.
func (mut t *T) RunWith(name string, body func(mut *T) !void, arg any) !void {
    if t.depth >= MaxDepth {
        return error.TooDeep
    }
    t.subs[t.depth] = name
    t.depth += 1
    outer := t.Arg
    t.Arg = arg

    body(t) catch |e| {
        if e != error.Failed && e != error.Skipped {
            try t.Failf("returned error {}", u64(e))
        }
    }

    t.Arg = outer
    t.depth -= 1
}

// The shared tally, so every task can add to it and the run can be reported
// once at the end.
struct Tally {
    failed  atomic[u64]
    skipped atomic[u64]
}

// One test, on its own task with its own memory, saying nothing to the terminal
// until it is done.
func runOne(cases []Case, at u64, tally *Tally, gate *shared[u64]) !void {
    mut backing := [ArenaSize]u8{}
    mut arena := mem.NewArena(backing[..])

    c := cases[at]
    mut t := try newT(c.Name, &arena)
    mut was_skipped := false
    c.Run(&t) catch |e| {
        if e == error.Skipped {
            was_skipped = true
        } else if e != error.Failed {
            try t.Failf("returned error {}", u64(e))
        }
    }

    if t.failed {
        tally.failed.Add(1)
    } else if was_skipped {
        tally.skipped.Add(1)
    }

    // The whole of what this test said, in one piece. Without the lock two
    // tests finishing together would shuffle their lines into each other.
    if t.out.Len() > 0 {
        lock held := gate {
            try io.Write(io.Stdout, string(t.out.Bytes()))
        }
    }
}

// `-p N` caps how many run at once; `-p 1` puts them back in order, which is
// what a test needing a port or a directory to itself wants.
func parallelism() u64 {
    mut room := [16]string{}
    args := os.Args(room[..])
    for i in 0..args.len {
        if args[i] != "-p" || i + 1 >= args.len {
            continue
        }
        want := strings.ParseU64(args[i+1]) catch 0
        if want > 0 {
            return want
        }
    }
    return DefaultParallel
}

// Runs every case and reports. The exit status is what `shield test` hands back
// to the shell: zero only when nothing failed.
func Run(cases []Case) !int {
    mut tally := Tally{failed: atomic[u64](0), skipped: atomic[u64](0)}
    mut gate := shared[u64](0)
    at_once := parallelism()
    start := time.Now()

    // A scope per batch rather than one for everything: it is the join that
    // bounds how many tasks — and how many stacks — exist at a time.
    mut from u64 = 0
    for from < cases.len {
        mut upto := from + at_once
        if upto > cases.len {
            upto = cases.len
        }
        scope {
            for i in from..upto {
                spawn runOne(cases, i, &tally, &gate)
            }
        }
        from = upto
    }

    took := time.Since(start)
    failed := tally.failed.Load()
    skipped := tally.skipped.Load()

    mut w := io.NewWriter(io.Stdout)
    if failed == 0 {
        try w.WriteString("ok    ")
    } else {
        try w.WriteString("FAIL  ")
    }
    try fmt.U64(&w, cases.len)
    try w.WriteString(" tests")
    if failed > 0 {
        try w.WriteString(", ")
        try fmt.U64(&w, failed)
        try w.WriteString(" failed")
    }
    if skipped > 0 {
        try w.WriteString(", ")
        try fmt.U64(&w, skipped)
        try w.WriteString(" skipped")
    }
    try w.WriteString("  (")
    try fmt.U64(&w, u64(took.AsMillis()))
    try w.WriteString("ms)\n")
    try w.Flush()

    if failed > 0 {
        return 1
    }
    return 0
}
