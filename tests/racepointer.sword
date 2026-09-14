// expect-error: this writes 'counter' while it is already written concurrently

func bump(mut p *i64) {
    *p = *p + 1
}

func main() int {
    mut counter i64 = 0
    scope {
        spawn bump(&counter)
        spawn bump(&counter)
    }
    return 0
}
