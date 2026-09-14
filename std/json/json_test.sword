package json

import "std/testing"

const src = "{\"name\":\"ada\",\"age\":36,\"tags\":[\"math\",\"engine\"],\"ok\":true,\"none\":null}"

func TestParseObject(mut t *testing.T) !void {
    mut doc := try Parse([]u8(src), t.Mem)
    root := doc.Root()
    try t.Equal(doc.Kind(root), Kind.Object)
    try t.Equal(doc.Len(root), 5)

    name := doc.Get(root, "name") orelse return t.Fatalf("no 'name'")
    try t.Equal(doc.Kind(name), Kind.Str)
    try t.Equal(doc.Text(name), "ada")

    age := doc.Get(root, "age") orelse return t.Fatalf("no 'age'")
    try t.Equal(doc.Kind(age), Kind.Number)
    try t.Equal(doc.Number(age), 36.0)

    ok := doc.Get(root, "ok") orelse return t.Fatalf("no 'ok'")
    try t.Equal(doc.Kind(ok), Kind.Bool)
    try t.Equal(doc.Truth(ok), true)

    none := doc.Get(root, "none") orelse return t.Fatalf("no 'none'")
    try t.Equal(doc.Kind(none), Kind.Null)

    if missing := doc.Get(root, "nobody") {
        try t.Failf("found a key that is not there")
    }
}

func TestParseArray(mut t *testing.T) !void {
    mut doc := try Parse([]u8(src), t.Mem)
    root := doc.Root()
    tags := doc.Get(root, "tags") orelse return t.Fatalf("no 'tags'")
    try t.Equal(doc.Kind(tags), Kind.Array)
    try t.Equal(doc.Len(tags), 2)

    first := doc.At(tags, 0) orelse return t.Fatalf("no first element")
    try t.Equal(doc.Text(first), "math")
    second := doc.At(tags, 1) orelse return t.Fatalf("no second element")
    try t.Equal(doc.Text(second), "engine")
    if past := doc.At(tags, 2) {
        try t.Failf("found an element past the end")
    }
}

func TestHelpers(mut t *testing.T) !void {
    mut doc := try Parse([]u8(src), t.Mem)
    root := doc.Root()
    try t.Equal(doc.GetText(root, "name"), "ada")
    try t.Equal(doc.GetText(root, "age"), "")
    try t.Equal(doc.GetNumber(root, "age", 0.0), 36.0)
    try t.Equal(doc.GetNumber(root, "name", -1.0), -1.0)
}

// Every one of these is malformed somewhere, and none of them may come back
// with a document.
func TestRejectsBadInput(mut t *testing.T) !void {
    bad := [7]string{"", "{", "[1,2", "{\"a\"}", "{\"a\":}", "tru", "{,}"}
    for s in bad {
        try t.RunWith(s, rejects, s)
    }
}

func rejects(mut t *testing.T) !void {
    Parse([]u8(t.Arg.Text), t.Mem) catch {
        return
    }
    try t.Failf("parsed it anyway")
}

func TestWriterRoundTrip(mut t *testing.T) !void {
    mut out := try bytes.New(t.Mem, 128)
    mut w := NewWriter(&out)
    try w.BeginObject()
    try w.Key("name")
    try w.Str("grace")
    try w.Key("age")
    try w.Number(45.0, 0)
    try w.EndObject()

    mut doc := try Parse([]u8(out.Str()), t.Mem)
    root := doc.Root()
    try t.Equal(doc.Kind(root), Kind.Object)
    try t.Equal(doc.GetText(root, "name"), "grace")
    try t.Equal(doc.GetNumber(root, "age", 0.0), 45.0)
}
