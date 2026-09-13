// expect: 110
// expect-output: caught


error NotEven = "the number is odd"
extern func write(fd i32, buf [*]u8, n u64) i64

func log(msg string) {
    write(1, msg.ptr, msg.len)
}

func half(n i32) !i32 {
    if n % 2 != 0 {
        return error.NotEven
    }
    return n / 2
}

// An error from the first `try` leaves this function without reaching the
// second one.
func quarter(n i32) !i32 {
    a := try half(n)
    return try half(a)
}

func main() int {
    good := quarter(40) catch 0
    bad := quarter(41) catch |e| {
        log("caught\n")
        return int(good) + 100
    }
    return int(good) + int(bad)
}
