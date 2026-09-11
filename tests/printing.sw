// expect: 0
// expect-output: 20 + 22 = 42
// Printing must not need an allocator: io.Printf formats into a block of
// stack and flushes as it fills.

import "std/io"

func main() !int {
    try io.Println("count:", 42, "ratio:", 1.5, true)
    try io.Printf("{} + {} = {}\n", 20, 22, 42)
    try io.Printf("hex={x} fixed={.3} str={}\n", 48879, 3.14159, "ok")
    try io.Printf("braces {{}} and a missing {}\n")
    try io.Printf("{} {} {}\n", u64(7), -9, 0.25)
    return 0
}
