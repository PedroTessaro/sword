// expect-error: every turn of this loop hands the same memory to a new task

func fill(mut xs []i64, v i64) {}

func main() !int {
    mut buf := [64]i64{}
    mut dst := buf[..]
    scope {
        for i in 0..8 {
            lo := i * 8
            spawn fill(dst[lo..lo+8], i64(i))
        }
    }
    return 0
}
