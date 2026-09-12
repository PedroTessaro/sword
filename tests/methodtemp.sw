// expect: 42
// A method's receiver is a struct, so a temporary already lives in memory:
// chaining needs nothing but its address.

struct Span {
    ns i64
}

func millis(n i64) Span {
    return Span{ns: n * 1000000}
}

func (s Span) AsMillis() i64 {
    return s.ns / 1000000
}

func (s Span) Add(other Span) Span {
    return Span{ns: s.ns + other.ns}
}

func main() int {
    if millis(20).AsMillis() != 20 {
        return 1
    }
    if millis(5).Add(millis(1000)).AsMillis() != 1005 {
        return 2
    }
    return 42
}
