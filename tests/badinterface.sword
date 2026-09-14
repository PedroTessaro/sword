// expect-error: Counter has no method 'close'

interface Closer {
    close() !void
}

struct Counter {
    total u64
}

func (c *Counter) value() u64 {
    return c.total
}

func shut(c Closer) !void {
    try c.close()
}

func main() int {
    mut c := Counter{total: 0}
    shut(&c) catch {}
    return 0
}
