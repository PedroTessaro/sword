// expect-error: package 'mem' has no exported function 'malloc'

import "std/mem"

func main() int {
    mem.malloc(8) orelse return 1
    return 0
}
