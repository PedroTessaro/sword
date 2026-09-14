// expect-error: this task would outlive the 'lock' it starts under
// A task started under a lock would still be running once the block releases
// it, so the scope has to sit inside the lock and not the other way round.

struct Tally {
    hits u64
}

func touch(mut c *Tally) {
    c.hits += 1
}

func main() int {
    mut tally := shared[Tally](Tally{hits: 0})
    scope {
        lock c := &tally {
            spawn touch(c)
        }
    }
    return 0
}
