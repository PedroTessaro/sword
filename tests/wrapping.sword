// expect: 0
// The wrapping operators opt out of the overflow check in every mode.

func main() int {
    mut n i32 = 2147483647
    n +%= 1
    if n == -2147483648 {
        return 0
    }
    return 1
}
