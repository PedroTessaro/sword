// expect-error: cannot be discarded; handle it with 'try' or 'catch'

func risky() !int {
    return error.Nope
}

func main() int {
    risky()
    return 0
}
