// expect: 34

func sum(xs []i32) i32 {
    mut total i32 = 0
    for i in 0..xs.len {
        total += xs[i]
    }
    return total
}

func scale(mut xs []i32, k i32) {
    for i in 0..xs.len {
        xs[i] *= k
    }
}

func main() int {
    mut nums := [4]i32{1, 2, 3, 4}
    mut view := nums[..]
    scale(view, 2)          // 2 4 6 8

    middle := nums[1..3]    // 4 6
    return int(sum(view)) + int(sum(middle)) + int(nums.len)
}
