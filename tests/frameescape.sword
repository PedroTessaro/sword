// expect-error: memory that lives in this function's frame
// The frame is gone before the caller can read it. This is the one shape of
// use-after-free the compiler can see without tracking lifetimes, so it does.

func leak() []u64 {
    mut room := [4]u64{}
    room[0] = 7
    return room[..]
}

func main() int {
    return int(leak()[0])
}
