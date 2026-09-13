// expect: 2
// An error that reaches main becomes the exit status. Codes are handed out in
// the order the names first appear, so Second is 2.


error First  = "the first thing that failed"
error Second = "the second thing that failed"
func first() !void {
    return error.First
}

func second() !void {
    return error.Second
}

func main() !void {
    first() catch {}
    try second()
}
