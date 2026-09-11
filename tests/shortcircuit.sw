// expect: 7
// The right side of && must not run when the left is false, and the
// division by zero below would trap if it did.

func div(a int, b int) int {
    return a / b
}

func main() int {
    n := 0
    mut score := 0
    if n != 0 && div(10, n) > 1 {
        score += 100
    }
    if n == 0 || div(10, n) > 1 {
        score += 7
    }
    return score
}
