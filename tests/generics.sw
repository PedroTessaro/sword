// expect: 98
// One body, instantiated per type: Max is compiled twice, Sum twice more.

func Max[T](a T, b T) T {
    if a > b {
        return a
    }
    return b
}

func Sum[T](xs []T) T {
    mut total := xs[0]
    for i in 1..xs.len {
        total += xs[i]
    }
    return total
}

func main() int {
    a := [4]i32{1, 2, 3, 4}
    b := [3]i64{10, 20, 30}

    x := Max[i32](7, 3)   // type argument written out
    y := Max(2, 9)        // inferred from the arguments
    s1 := Sum(a[..])
    s2 := Sum(b[..])

    return int(x) + int(y) + int(s1) + int(s2) +
           int(sizeof[i64]()) + int(alignof[i32]())
}
