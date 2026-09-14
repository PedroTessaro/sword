// expect-error: expects func([]i64), got func(mut []i64)
// Write access is part of a function's type: handing over one that writes
// where the caller expected one that does not would grant permission nobody
// asked for.

func writes(mut xs []i64) {
    xs[0] = 1
}

func take(op func([]i64)) {}

func main() int {
    take(writes)
    return 0
}
