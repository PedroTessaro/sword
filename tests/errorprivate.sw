// expect-error: is not exported by
// An error whose name starts lowercase belongs to the package that declared it,
// the same as a function, a type or a field.

import "std/testing"

func main() !int {
    return int(u64(nameof(error.failed).len))
}
