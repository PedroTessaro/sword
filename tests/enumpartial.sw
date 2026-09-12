// expect-error: does not cover Bool, Int, Text
// An enum is a closed set, so a switch over one with no 'default' has to
// account for every member.

enum Kind {
    None
    Bool
    Int
    Text
}

func describe(k Kind) int {
    switch k {
    case Kind.None:
        return 0
    }
    return 1
}

func main() int {
    return describe(Kind.Int)
}
