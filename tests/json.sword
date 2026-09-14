// expect: 42
// expect-output: json works
// Objects, arrays, escapes, nesting, and malformed input rejected rather
// than crashed on.

import "std/bytes"
import "std/fmt"
import "std/io"
import "std/json"
import "std/mem"
import "std/strings"

func main() !int {
    mut room := [262144]u8{}
    mut arena := mem.NewArena(room[..])

    src := "{\"name\":\"sword\",\"version\":0.1,\"tags\":[\"fast\",\"safe\"],\"ok\":true,\"nothing\":null,\"nested\":{\"depth\":2}}"
    mut doc := try json.Parse([]u8(src), &arena)
    root := doc.Root()

    if doc.Kind(root) != json.Kind.Object {
        return 1
    }
    if !strings.Equal(doc.GetText(root, "name"), "sword") {
        return 2
    }
    v := doc.GetNumber(root, "version", 0.0)
    if v < 0.09 || v > 0.11 {
        return 3
    }

    tags := doc.Get(root, "tags") orelse return 4
    if doc.Kind(tags) != json.Kind.Array || doc.Len(tags) != 2 {
        return 5
    }
    second := doc.At(tags, 1) orelse return 6
    if !strings.Equal(doc.Text(second), "safe") {
        return 7
    }

    flag := doc.Get(root, "ok") orelse return 8
    if !doc.Truth(flag) {
        return 9
    }
    empty := doc.Get(root, "nothing") orelse return 10
    if doc.Kind(empty) != json.Kind.Null {
        return 11
    }
    nested := doc.Get(root, "nested") orelse return 12
    if doc.GetNumber(nested, "depth", 0.0) != 2.0 {
        return 13
    }
    if found := doc.Get(root, "missing") {
        return 14
    }

    // Escapes are unescaped into the allocator.
    mut esc := try json.Parse([]u8("{\"s\":\"a\\nb\\u00e9\"}"), &arena)
    text := esc.GetText(esc.Root(), "s")
    if text.len != 5 || text[1] != 10 {
        return 15
    }

    // Malformed input is an error, not a crash.
    json.Parse([]u8("{\"a\":"), &arena) catch {
        json.Parse([]u8("[1,2"), &arena) catch {
            try io.Print("json works\n")
            return 42
        }
        return 16
    }
    return 17
}
