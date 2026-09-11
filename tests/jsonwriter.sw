// expect: 42
// expect-output: json writer works
// What the writer emits has to parse back to the same document.

import "std/bytes"
import "std/io"
import "std/json"
import "std/mem"
import "std/strings"

func main() !int {
    mut room := [65536]u8{}
    mut arena := mem.NewArena(room[..])
    mut b := try bytes.New(&arena, 128)
    mut w := json.NewWriter(&b)

    try w.BeginObject()
    try w.Key("name")
    try w.Str("swo\"rd")
    try w.Key("version")
    try w.Number(0.1, 1)
    try w.Key("count")
    try w.Int(-3)
    try w.Key("tags")
    try w.BeginArray()
    try w.Str("fast")
    try w.Str("safe")
    try w.EndArray()
    try w.Key("ok")
    try w.Bool(true)
    try w.Key("nothing")
    try w.Null()
    try w.EndObject()

    want := "{\"name\":\"swo\\\"rd\",\"version\":0.1,\"count\":-3,\"tags\":[\"fast\",\"safe\"],\"ok\":true,\"nothing\":null}"
    if !strings.Equal(b.Str(), want) {
        try io.Print(b.Str())
        try io.Print("\n")
        return 1
    }

    // What was written has to parse back to the same thing.
    mut doc := try json.Parse([]u8(b.Str()), &arena)
    if !strings.Equal(doc.GetText(doc.Root(), "name"), "swo\"rd") {
        return 2
    }
    try io.Print("json writer works\n")
    return 42
}
