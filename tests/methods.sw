// expect: 42

struct Counter {
    hits u64
}

func (c *Counter) value() u64 {
    return c.hits
}

// A `mut` receiver may write through itself, and only a mutable binding may
// hand one out.
func (mut c *Counter) bump(by u64) {
    c.hits += by
}

func main() int {
    mut c := Counter{hits: 1}
    c.bump(20)
    c.bump(c.value())
    return int(c.value())
}
