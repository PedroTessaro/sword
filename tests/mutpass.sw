// expect-error: cannot pass immutable binding 'view'

func scale(mut xs []i32, k i32) {
    xs[0] *= k
}

func main() int {
    mut nums := [2]i32{1, 2}
    view := nums[..]
    scale(view, 2)
    return 0
}
