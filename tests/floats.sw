// expect: 42

const billion = 1000000000

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

    // A literal converted to a float keeps its value: it stops being an
    // integer at the conversion, not at the machine instruction.
    if f64(1000000000) != 1.0e9 {
        return 5
    }
    if f64(billion) / f64(4) != 2.5e8 {
        return 6
    }
    mut sum := 1.5
    sum += 2
    if sum != 3.5 || i64(2.75) != 2 {
        return 7
    }
    return 42
}
