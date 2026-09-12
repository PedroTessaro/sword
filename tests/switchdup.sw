// expect-error: this case is already covered
// Two cases matching the same value means one of them is dead.

func main() int {
    x := 3
    switch x {
    case 1:
        return 1
    case 2, 1:
        return 2
    default:
        return 0
    }
}
