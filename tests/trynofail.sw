// expect-error: 'try' propagates an error, so this function must return '!'

func risky() !int {
    return error.Nope
}

func main() int {
    n := try risky()
    return int(n)
}
