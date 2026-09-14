// expect-error: 'tally' is already locked here
// Taking the same lock twice on one thread is a hang, so the checker says so
// where it can see it; the runtime catches the case that goes through a call.

struct Tally {
    hits u64
}

func main() int {
    mut tally := shared[Tally](Tally{hits: 0})
    lock a := &tally {
        lock b := &tally {
            b.hits += 1
        }
    }
    return 0
}
