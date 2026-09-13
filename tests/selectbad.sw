// expect-error: a select with no cases has nothing to wait for
// A select with nothing in it waits for nothing in particular, forever. Go makes
// that a deadlock at run time; there is no reason to wait until then.

func main() int {
    select {
    }
    return 0
}
