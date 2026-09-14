// expect-error: writes through its receiver
// Writing into a value nobody kept can only be a mistake.

struct Counter {
    hits u64
}

func fresh() Counter {
    return Counter{hits: 0}
}

func (mut c *Counter) Bump() {
    c.hits += 1
}

func main() int {
    fresh().Bump()
    return 0
}
