// expect-error: cannot return out of a 'scope'

func work(n i64) {}

func main() int {
    scope {
        for i in 0..4 {
            spawn work(i)
            if i == 2 {
                return 1
            }
        }
    }
    return 0
}
