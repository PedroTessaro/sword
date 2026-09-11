// expect-error: i64 cannot have methods
// The signature pass gives up here; checking the body would have crashed.

func (x *int) Twice() int {
    return x * 2
}

func main() int {
    return 0
}
