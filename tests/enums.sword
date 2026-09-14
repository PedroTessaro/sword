// expect: 42
// expect-output: enums work
// A named set of integers with its own type, and a switch over one that has to
// account for every member.

import "shapes"
import "std/io"

enum Kind u8 {
    None
    Bool
    Int
    Text = 9
    Real
}

// No `default`, and it still returns on every path: the switch covers the set.
func describe(k Kind) string {
    switch k {
    case Kind.None:
        return "none"
    case Kind.Bool, Kind.Int:
        return "number-ish"
    case Kind.Text:
        return "text"
    case Kind.Real:
        return "real"
    }
}

struct Slot {
    kind  Kind
    value i64
}

func main() !int {
    k := Kind.Int
    if k != Kind.Int || k == Kind.None {
        return 1
    }
    if describe(Kind.None) != "none" || describe(Kind.Real) != "real" {
        return 2
    }
    if describe(Kind.Bool) != "number-ish" {
        return 3
    }

    // A member with no value continues from the one before it.
    if u8(Kind.None) != 0 || u8(Kind.Int) != 2 || u8(Kind.Real) != 10 {
        return 4
    }
    // And the way back in, for a value that came off the wire.
    if Kind(9) != Kind.Text {
        return 5
    }

    // In a struct, in an array, and walked.
    slot := Slot{kind: Kind.Text, value: 7}
    if slot.kind != Kind.Text {
        return 6
    }
    kinds := [3]Kind{Kind.None, Kind.Bool, Kind.Int}
    mut seen := 0
    for one in kinds {
        switch one {
        case Kind.None:
            seen += 1
        case Kind.Bool:
            seen += 10
        default:
            seen += 100
        }
    }
    if seen != 111 {
        return 7
    }

    // An enum from another package, named through it.
    corner := shapes.Corner.Round
    if corner != shapes.Corner.Round || corner == shapes.Corner.Sharp {
        return 8
    }
    switch corner {
    case shapes.Corner.Sharp:
        return 9
    case shapes.Corner.Round:
        seen += 0
    case shapes.Corner.Bevelled:
        return 10
    }

    // A member's name, and the empty string for a value that is not one.
    if nameof(Kind.Real) != "Real" || nameof(k) != "Int" {
        return 11
    }
    if nameof(Kind(200)) != "" {
        return 12
    }
    if nameof(shapes.Corner.Bevelled) != "Bevelled" {
        return 13
    }
    // Printing one gives the name, because the number would be useless.
    try io.Printf("enums work: {} is {}, {} is {}\n", nameof(Kind.Real),
                  describe(Kind.Real), Kind.Text, u8(Kind.Text))
    return 42
}
