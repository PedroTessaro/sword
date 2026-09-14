// expect-error: 'try' propagates an error, so this function must return '!'


error Nope = "this is the failure the test is about"
func risky() !int {
    return error.Nope
}

func main() int {
    n := try risky()
    return int(n)
}
