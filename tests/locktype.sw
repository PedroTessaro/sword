// expect-error: 'lock' needs a shared value
// `lock` is the only way into a shared, and it is not a way into anything else.

struct Tally {
    hits u64
}

func main() int {
    mut tally := Tally{hits: 0}
    lock c := &tally {
        c.hits += 1
    }
    return 0
}
