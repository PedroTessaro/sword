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

    // A plain value assigned into an optional is wrapped where it is stored,
    // so the literal itself stays a plain u64.
    mut count ?u64 = nil
    count = 5
    if (count orelse 0) != 5 {
        return 96
    }

    mut buf := malloc(16) orelse return 91
    buf[0] = 2
    total := int(hit.key) + int(miss.key) + int(buf[0])
    free(buf)
    return total
}
