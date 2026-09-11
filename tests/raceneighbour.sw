// expect-error: 'xs' is written by this loop, so it can only be reached as 'xs[i]'

func main() int {
    mut buf := [8]i64{1, 2, 3, 4, 5, 6, 7, 8}
    mut xs := buf[..]
    parallel for i in 0..7 {
        xs[i] = xs[i+1]
    }
    return 0
}
