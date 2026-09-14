// expect: 42
// A loop over elements, with and without the position, over all three things
// that have elements.

func main() int {
    values := [5]i64{3, 1, 4, 1, 5}
    mut total i64 = 0
    for v in values {
        total += v
    }
    if total != 14 {
        return 1
    }

    view := values[..]
    mut weighted i64 = 0
    for i, v in view {
        weighted += i64(i) * v
    }
    if weighted != 32 {
        return 2
    }

    mut sum u64 = 0
    for c in "hello" {
        sum += u64(c)
    }
    if sum != 532 {
        return 3
    }

    // The element is a copy, so writing it changes nothing behind it.
    mut room := [3]i64{1, 2, 3}
    for v in room {
        mut copy := v
        copy += 100
    }
    if room[0] != 1 {
        return 4
    }

    // break and continue still speak to the loop
    mut first i64 = 0
    for v in view {
        if v == 1 {
            continue
        }
        first = v
        break
    }
    if first != 3 {
        return 5
    }

    // A slice of an array walks the part, not the whole
    mut counted := 0
    for v in values[1..3] {
        counted += 1
    }
    if counted != 2 {
        return 6
    }
    return 42
}
