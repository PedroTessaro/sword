// expect: 45

extern func malloc(n u64) ?[*]u8
extern func free(p [*]u8)

struct Node {
    key  i32
    next ?*Node
}

// `?*Node` costs nothing: absence is the null pointer, so a Node is still
// two words wide.
func find(head ?*Node, key i32) ?*Node {
    mut cur := head
    for {
        if n := cur {
            if n.key == key {
                return n
            }
            cur = n.next
        } else {
            return nil
        }
    }
}

func main() int {
    mut c := Node{key: 3, next: nil}
    mut b := Node{key: 2, next: &c}
    mut a := Node{key: 1, next: &b}

    hit := find(&a, 3) orelse return 90

    mut fallback := Node{key: 40, next: nil}
    miss := find(&a, 9) orelse &fallback

    // Comparing against nil asks whether it is absent, which works for both
    // shapes: the pointer one and the { has, value } one.
    if find(&a, 9) != nil {
        return 92
    }
    if find(&a, 3) == nil {
        return 93
    }
    mut count ?u64 = nil
    if count != nil {
        return 94
    }
    count = 5
    if count == nil || nil == count {
        return 95
    }
    if (count orelse 0) != 5 {
        return 96
    }

    mut buf := malloc(16) orelse return 91
    buf[0] = 2
    total := int(hit.key) + int(miss.key) + int(buf[0])
    free(buf)
    return total
}
