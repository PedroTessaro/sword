// expect-error: 'total' is the reduction variable

func main() int {
    mut total i64 = 0
    parallel for i in 0..4 reduce(+: total) {
        mut total i64 = 5
        total += 1
    }
    return int(total)
}
