// expect-error: while it is already read concurrently
// The join is the closing brace, so the parent's own writes inside the block
// run alongside the tasks.

func read(xs []i64) {}

func main() !int {
    mut buf := [4]i64{1, 2, 3, 4}
    mut xs := buf[..]
    scope {
        spawn read(xs)
        xs[0] = 9
    }
    return 0
}
