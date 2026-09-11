// expect: 49
// A type written after the last name in a group covers all of them, as in Go.
// `mut` stays attached to the name it precedes.

struct Point {
    x, y  i32
    label string
}

func area(w, h u64) u64 {
    return w * h
}

func scale(mut xs, mut ys []i64, k i64) {
    for i in 0..xs.len {
        xs[i] *= k
        ys[i] *= k
    }
}

func main() int {
    p := Point{x: 3, y: 4, label: "p"}
    mut a := [2]i64{1, 2}
    mut b := [2]i64{3, 4}
    mut va := a[..]
    mut vb := b[..]
    scale(va, vb, 2)
    return int(area(5, 6)) + int(p.x) + int(p.y) + int(va[1]) + int(vb[1])
}
