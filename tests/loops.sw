// expect: 100

func main() int {
    mut total := 0
    for i in 0..10 {
        mut inner := 0
        for {
            if inner >= 10 {
                break
            }
            inner += 1
        }
        total += inner
    }
    return total
}
