// expect: 0
// expect-output: hello from sword

extern func write(fd i32, buf [*]u8, n u64) i64

func main() int {
    msg := "hello from sword\n"
    write(1, msg.ptr, msg.len)
    return 0
}
