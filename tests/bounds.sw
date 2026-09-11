// expect: 134
// Out of bounds aborts in safe mode; 134 is SIGABRT.

func main() int {
    nums := [3]i32{1, 2, 3}
    view := nums[..]
    mut i := 0
    for {
        i += 1
        if view[i] == 0 {
            return 1
        }
    }
}
