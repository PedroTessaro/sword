// expect-error: cannot be discarded; handle it with 'try' or 'catch'


error Nope = "this is the failure the test is about"
func risky() !int {
    return error.Nope
}

func main() int {
    risky()
    return 0
}
