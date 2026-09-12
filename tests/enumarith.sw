// expect-error: needs numeric operands, got Kind
// An enum has its own type, so nothing arithmetic reaches it by accident.

enum Kind { None, Bool }

func main() int {
    k := Kind.None
    return int(k + Kind.Bool)
}
