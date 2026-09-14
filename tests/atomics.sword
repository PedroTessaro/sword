// expect: 42
// expect-output: atomics work
// An atomic may be written by any number of tasks at once; that is the one
// exemption the race checker makes.

import "std/io"
import "std/mem"

func bump(mut hits *atomic[u64], n u64) {
    for i in 0..n {
        hits.Add(1)
    }
}

func main() !int {
    mut hits := atomic[u64](0)
    mut flag := atomic[bool](false)

    scope {
        for i in 0..8 {
            spawn bump(&hits, 1000)
        }
    }

    if hits.Load() != 8000 {
        return 1
    }
    if hits.Swap(5) != 8000 || hits.Load() != 5 {
        return 2
    }
    if !hits.CompareSwap(5, 9) || hits.Load() != 9 {
        return 3
    }
    if hits.CompareSwap(5, 1) {
        return 4
    }
    if hits.Sub(4) != 9 || hits.Load() != 5 {
        return 5
    }
    flag.Store(true)
    if !flag.Load() {
        return 6
    }
    try io.Print("atomics work\n")
    return 42
}
