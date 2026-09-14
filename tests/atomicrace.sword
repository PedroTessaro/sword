// expect-error: this writes 'counter' while it is already written concurrently
// A plain integer gets no exemption: only an atomic may be shared for writing.

func bump(mut p *u64) {
    *p = *p + 1
}

func main() int {
    mut counter u64 = 0
    scope {
        spawn bump(&counter)
        spawn bump(&counter)
    }
    return 0
}
