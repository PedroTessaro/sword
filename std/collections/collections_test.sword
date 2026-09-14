package collections

import "std/mem"
import "std/testing"

// `t.Mem` is the test's own arena, reset before each test, so nothing here
// frees anything.

func TestListGrows(mut t *testing.T) !void {
    mut xs := try NewList[i64](t.Mem, 2)
    try t.Equal(xs.Len(), 0)
    try t.Equal(xs.Cap(), MinCapacity)

    for i in 0..100 {
        try xs.Push(i64(i) * 2)
    }
    try t.Equal(xs.Len(), 100)
    try t.Equal(xs.At(0), 0)
    try t.Equal(xs.At(99), 198)

    // The window onto the live elements follows the length, not the capacity.
    try t.Equal(xs.Slice().len, 100)

    last := xs.Pop() orelse return t.Fatalf("popped nothing")
    try t.Equal(last, 198)
    try t.Equal(xs.Len(), 99)

    xs.Reset()
    try t.Equal(xs.Len(), 0)
    if empty := xs.Pop() {
        try t.Failf("popped from an empty list")
    }
}

func TestMapSetAndGet(mut t *testing.T) !void {
    mut m := try NewMap[u64](t.Mem, 8)
    try t.Equal(m.Len(), 0)
    if missing := m.Get("nobody") {
        try t.Failf("found a key that was never set")
    }

    try m.Set("ada", 1)
    try m.Set("grace", 2)
    try m.Set("alan", 3)
    try t.Equal(m.Len(), 3)
    try t.Equal(m.Get("ada") orelse 0, 1)
    try t.Equal(m.Get("alan") orelse 0, 3)
    try t.Equal(m.Has("grace"), true)
    try t.Equal(m.Has("edsger"), false)

    // Setting an existing key replaces rather than adds.
    try m.Set("ada", 9)
    try t.Equal(m.Len(), 3)
    try t.Equal(m.Get("ada") orelse 0, 9)

    try t.Equal(m.Delete("grace"), true)
    try t.Equal(m.Delete("grace"), false)
    try t.Equal(m.Len(), 2)
    if gone := m.Get("grace") {
        try t.Failf("found a deleted key")
    }
}

// Past the load factor the table has to rehash, and everything in it has to
// survive that.
func TestMapRehashes(mut t *testing.T) !void {
    mut m := try NewMap[u64](t.Mem, 8)
    mut keys := [64]string{}
    for i in 0..64 {
        keys[i] = try label(t, u64(i))
        try m.Set(keys[i], u64(i))
    }
    try t.Equal(m.Len(), 64)
    try t.Require(m.Slots() > 64)

    for i, key in keys {
        try t.Equal(m.Get(key) orelse 999, u64(i))
    }
}

// `k0`, `k1`, ... out of the test's own memory. Digits come out backwards,
// which does not matter: the keys only have to differ.
func label(mut t *testing.T, i u64) !string {
    mut room := mem.Alloc[u8](t.Mem, 8) orelse return error.OutOfMemory
    room[0] = 107 // 'k'
    mut n := i
    mut at u64 = 1
    if n == 0 {
        room[at] = 48
        at += 1
    }
    for n > 0 {
        room[at] = 48 + u8(n % 10)
        n = n / 10
        at += 1
    }
    return string(room[0..at])
}
