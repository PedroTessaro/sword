// expect-error: a spawned function cannot return a value

func compute(n i64) i64 {
    return n * 2
}

func main() int {
    scope {
        spawn compute(21)
    }
    return 0
}
