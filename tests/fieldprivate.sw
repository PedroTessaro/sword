// expect-error: field 'ns' of time.Duration is not exported by its package
// A package publishing a type used to publish everything inside it. `Duration`
// keeps its nanoseconds to itself, and `Millis`, `AsMillis` and the rest are the
// way in.

import "std/io"
import "std/time"

func main() !int {
    d := time.Millis(5)
    try io.Printf("{}\n", d.ns)
    return 0
}
