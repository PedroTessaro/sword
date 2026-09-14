// expect: 42
// expect-output: any parameters work
// A plain `any` parameter, as opposed to a gathered `...any`: the value has to
// be boxed at the call site, the same way an interface pair is built there.

import "std/io"

func describe(v any) string {
    switch v.Kind {
    case 1:
        return "bool"
    case 2, 3:
        return "integer"
    case 4:
        return "float"
    case 5:
        return "string"
    default:
        return "other"
    }
}

func same(a any, b any) bool {
    if a.Kind != b.Kind {
        return false
    }
    if a.Kind == 5 {
        return a.Text == b.Text
    }
    return a.Int == b.Int
}

func main() !int {
    if describe(true) != "bool" || describe(7) != "integer" {
        return 1
    }
    if describe(1.5) != "float" || describe("x") != "string" {
        return 2
    }
    if !same(7, 7) || same(7, 8) || !same("a", "a") || same("a", "b") {
        return 3
    }
    // And a boxed value passed straight on, without boxing it twice.
    if pass(7) != "integer" {
        return 4
    }
    try io.Print("any parameters work\n")
    return 42
}

func pass(v any) string {
    return describe(v)
}
