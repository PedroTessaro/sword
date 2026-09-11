// expect: 21

struct Counter {
    hits u64
}

func bump(mut p *u64, by u64) {
    *p = *p + by
}

func main() int {
    mut c := Counter{hits: 0}
    mut n u64 = 7

    bump(&c.hits, 14)
    bump(&n, 0)
    return int(c.hits) + int(n) - 0
}
