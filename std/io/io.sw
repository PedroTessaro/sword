package io

extern func write(fd i32, buf [*]u8, n u64) i64

func Write(fd i32, s string) !u64 {
    n := write(fd, s.ptr, s.len)
    if n < 0 {
        return error.WriteFailed
    }
    return u64(n)
}

func Print(s string) !void {
    try Write(1, s)
}

func Fail(s string) !void {
    try Write(2, s)
}
