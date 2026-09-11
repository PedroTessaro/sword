// expect: 42
// Gathering, forwarding with `xs...`, and boxing into `any`.

func sum(label string, xs ...i64) i64 {
    mut total i64 = 0
    for i in 0..xs.len {
        total += xs[i]
    }
    return total + i64(label.len)
}

func kinds(xs ...any) i64 {
    mut acc i64 = 0
    for i in 0..xs.len {
        acc = acc * 10 + i64(xs[i].Kind)
    }
    return acc
}

func forward(xs ...any) i64 {
    return kinds(xs...)
}

func main() int {
    if sum("ab", 1, 2, 3) != 8 {
        return 1
    }
    if sum("x") != 1 {
        return 2
    }
    // bool, int, uint, float, string
    if kinds(true, 7, u64(9), 1.5, "hi") != 12345 {
        return 3
    }
    if forward(1, "a") != 25 {
        return 4
    }
    mut n := 5
    if kinds(&n) != 6 {
        return 5
    }
    return 42
}
