// expect: 42
// expect-output: switch works
// A switch on integers, strings and bools, plus string equality as an operator.

import "std/io"

const Created = 201

func statusText(code u64) string {
    switch code {
    case 200:
        return "OK"
    case Created:
        return "Created"
    case 400, 404, 405:
        return "bad request"
    default:
        return "unknown"
    }
}

func methodKind(m string) int {
    switch m {
    case "GET", "HEAD":
        return 1
    case "POST", "PUT":
        return 2
    default:
        return 0
    }
}

func main() !int {
    if !strings_ok() {
        return 1
    }
    if statusText(200) != "OK" || statusText(201) != "Created" {
        return 2
    }
    if statusText(404) != "bad request" || statusText(999) != "unknown" {
        return 3
    }
    if methodKind("HEAD") != 1 || methodKind("PUT") != 2 {
        return 4
    }
    if methodKind("BREW") != 0 {
        return 5
    }

    // A switch is not a loop: break belongs to the loop around it.
    mut found := 0
    for i in 0..10 {
        switch i {
        case 3:
            found = i
            break
        default:
            found = -1
        }
        if found > 0 {
            break
        }
    }
    if found != 3 {
        return 6
    }

    // No default: nothing matching means nothing happens.
    mut touched := 0
    switch 7 {
    case 1, 2:
        touched = 1
    }
    if touched != 0 {
        return 7
    }

    flag := true
    switch flag {
    case true:
        touched = 9
    case false:
        touched = 8
    }
    if touched != 9 {
        return 8
    }

    try io.Print("switch works\n")
    return 42
}

func strings_ok() bool {
    if "abc" != "abc" {
        return false
    }
    if "abc" == "abd" || "ab" == "abc" {
        return false
    }
    empty := ""
    if empty != "" {
        return false
    }
    return true
}
