// expect: 0
// expect-output: abc-doxx-DED|caught

extern func write(fd i32, buf [*]u8, n u64) i64

func put(s string) {
    write(1, s.ptr, s.len)
}

// Deferred statements are LIFO within a scope, and a block scope ends at its
// own closing brace: "c" comes before "-", but "d" comes after it.
func scopes() {
    put("a")
    defer put("d")
    {
        defer put("c")
        put("b")
    }
    put("-")
}

// A defer in a loop body runs on every iteration, including the one that
// breaks out.
func loops() {
    for i in 0..3 {
        defer put("x")
        if i == 1 {
            break
        }
        put("o")
    }
    put("-")
}

// errdefer runs only when the scope is left through an error, and shares the
// LIFO order with defer: declared second, so it runs first.
func step(fail bool) !void {
    defer put("D")
    errdefer put("E")
    if fail {
        return error.Boom
    }
}

func both() !void {
    try step(false)
    try step(true)
}

func main() int {
    scopes()
    loops()
    both() catch |e| {
        put("|caught")
        return 0
    }
    return 1
}
