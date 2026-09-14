// expect: 42
// expect-output: map works
// Open addressing with linear probing, through several rehashes, with
// tombstones and iteration by slot.

import "std/collections"
import "std/io"
import "std/mem"
import "std/strings"

func main() !int {
    mut room := [262144]u8{}
    mut arena := mem.NewArena(room[..])

    mut ages := try collections.NewMap[i64](&arena, 4)
    try ages.Set("ana", 31)
    try ages.Set("beto", 47)
    try ages.Set("ana", 32)

    if ages.Len() != 2 {
        return 1
    }
    got := ages.Get("ana") orelse return 2
    if got != 32 {
        return 3
    }
    if ages.Has("ninguem") {
        return 4
    }

    // Enough entries to force several rehashes.
    mut keys := try collections.NewList[string](&arena, 8)
    mut names := [4]u8{}
    for i in 0..200 {
        names[0] = u8(97 + i % 26)
        names[1] = u8(97 + (i / 26) % 26)
        names[2] = u8(97 + (i / 676) % 26)
        mut key := mem.Alloc[u8](&arena, 3) orelse return 5
        for k in 0..3 {
            key[k] = names[k]
        }
        try keys.Push(string(key))
        try ages.Set(string(key), i64(i))
    }
    if ages.Len() != 202 {
        return 6
    }
    for i in 0..keys.Len() {
        v := ages.Get(keys.At(i)) orelse return 7
        if v != i64(i) {
            return 8
        }
    }

    if !ages.Delete("beto") || ages.Has("beto") {
        return 9
    }

    mut live u64 = 0
    for i in 0..ages.Slots() {
        if k := ages.KeyAt(i) {
            live += 1
        }
    }
    if live != ages.Len() {
        return 10
    }
    try io.Print("map works\n")
    return 42
}
