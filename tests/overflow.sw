// expect: 134

func main() int {
    mut n i32 = 2147483000
    for {
        n += 1000
    }
}
