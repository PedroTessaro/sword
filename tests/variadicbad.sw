// expect-error: only the last parameter can gather the rest

func bad(xs ...i64, tail string) i64 {
    return 0
}

func main() int {
    return int(bad(1, "x"))
}
