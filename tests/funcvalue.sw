// expect: 42
// expect-output: function values work
// A function named without being called is its address. No closures, so
// nothing is captured and nothing is allocated.

import "std/io"

func add(a, b i64) i64 {
    return a + b
}

func mul(a, b i64) i64 {
    return a * b
}

func double(mut xs []i64) {
    for i in 0..xs.len {
        xs[i] *= 2
    }
}

func apply(op func(i64, i64) i64, a i64, b i64) i64 {
    return op(a, b)
}

// A function value in a field is called the same way a method would be.
struct Op {
    name string
    run  func(i64, i64) i64
}

func main() !int {
    if apply(add, 20, 22) != 42 || apply(mul, 6, 7) != 42 {
        return 1
    }

    ops := [2]Op{Op{name: "add", run: add}, Op{name: "mul", run: mul}}
    mut folded i64 = 0
    for o in ops {
        folded += o.run(3, 4)
    }
    if folded != 19 {
        return 2
    }

    f := add
    mut g := mul
    g = add
    if f(1, 2) != 3 || g(1, 2) != 3 {
        return 3
    }

    // A `mut` parameter travels with the type, and still needs permission.
    changer := double
    mut room := [3]i64{1, 2, 3}
    mut view := room[..]
    changer(view)
    if view[0] != 2 || view[2] != 6 {
        return 4
    }

    // `?func(...)` is the pointer, with null standing for absence.
    mut maybe ?func(i64, i64) i64 = nil
    if first := maybe {
        return 5
    }
    maybe = mul
    if chosen := maybe {
        if chosen(5, 5) != 25 {
            return 6
        }
    } else {
        return 7
    }

    try io.Print("function values work\n")
    return 42
}
