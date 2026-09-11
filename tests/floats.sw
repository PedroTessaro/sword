// expect: 42

func area(r f64) f64 {
    return 3.14159265358979 * r * r
}

func main() int {
    a := area(2.0)
    if a < 12.56 || a > 12.57 {
        return 1
    }

    mut acc f32 = 0.5
    acc += 0.25
    acc *= 4.0
    if acc != 3.0 {
        return 2
    }

    // Mixing widths needs a conversion, both ways.
    n := 7
    half := f64(n) / 2.0
    if half != 3.5 || int(half) != 3 {
        return 3
    }
    if -f64(acc) != -3.0 {
        return 4
    }
    return 42
}
