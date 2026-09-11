package collections

import "std/mem"

// Open addressing with linear probing. One flat array of entries, no buckets
// and no per-entry allocation: the whole table is a single block from the
// allocator, which is what keeps lookups cache-friendly.
//
// Keys are strings, which is what the callers in this library need. A key
// type parameter would need a hash and an equality constraint, and there is
// nowhere yet to put them.
struct Entry[V] {
    key   string
    value V
    state u8
}

const slotEmpty = 0
const slotLive = 1
const slotDead = 2

struct Map[V] {
    slots []Entry[V]
    count u64
    a     mem.Allocator
}

const MinSlots = 16

// FNV-1a: small, fast, and good enough for short keys like header names. The
// multiply is meant to overflow, which is what the wrapping operator says.
func Hash(key string) u64 {
    mut h u64 = 14695981039346656037
    for i in 0..key.len {
        h ^= u64(key[i])
        h *%= 1099511628211
    }
    return h
}

func keysEqual(a string, b string) bool {
    if a.len != b.len {
        return false
    }
    for i in 0..a.len {
        if a[i] != b[i] {
            return false
        }
    }
    return true
}

func NewMap[V](mut a mem.Allocator, capacity u64) !Map[V] {
    mut want u64 = MinSlots
    for want < capacity * 2 {
        want *= 2
    }
    mut room := mem.Alloc[Entry[V]](a, want) orelse
        return error.OutOfMemory
    for i in 0..room.len {
        room[i].state = slotEmpty
    }
    return Map[V]{slots: room, count: 0, a: a}
}

func (m *Map[V]) Len() u64 {
    return m.count
}

// The index of `key`, or of the first free slot where it would go. The table
// is never allowed to fill up, so this always terminates.
func (m *Map[V]) probe(key string) u64 {
    mask := m.slots.len - 1
    mut at := Hash(key) & mask
    for {
        state := m.slots[at].state
        if state == slotEmpty {
            return at
        }
        if state == slotLive && keysEqual(m.slots[at].key, key) {
            return at
        }
        at = (at + 1) & mask
    }
}

func (m *Map[V]) Get(key string) ?V {
    at := m.probe(key)
    if m.slots[at].state != slotLive {
        return nil
    }
    return m.slots[at].value
}

func (m *Map[V]) Has(key string) bool {
    at := m.probe(key)
    return m.slots[at].state == slotLive
}

// Grows past a load factor of one half, which is where linear probing starts
// to cluster badly.
func (mut m *Map[V]) grow() !void {
    old := m.slots
    mut room := mem.Alloc[Entry[V]](m.a, old.len * 2) orelse
        return error.OutOfMemory
    for i in 0..room.len {
        room[i].state = slotEmpty
    }
    m.slots = room
    m.count = 0
    for i in 0..old.len {
        if old[i].state == slotLive {
            try m.Set(old[i].key, old[i].value)
        }
    }
    mem.Free(m.a, old)
}

func (mut m *Map[V]) Set(key string, value V) !void {
    if (m.count + 1) * 2 > m.slots.len {
        try m.grow()
    }
    at := m.probe(key)
    if m.slots[at].state != slotLive {
        m.count += 1
    }
    m.slots[at].key = key
    m.slots[at].value = value
    m.slots[at].state = slotLive
}

// A removed slot stays as a tombstone so that probes past it still find what
// comes after.
func (mut m *Map[V]) Delete(key string) bool {
    at := m.probe(key)
    if m.slots[at].state != slotLive {
        return false
    }
    m.slots[at].state = slotDead
    m.count -= 1
    return true
}

func (mut m *Map[V]) Free() {
    mem.Free(m.a, m.slots)
    m.count = 0
}

// Iteration is by slot index: walk 0..Slots() and ask each one.
func (m *Map[V]) Slots() u64 {
    return m.slots.len
}

func (m *Map[V]) KeyAt(i u64) ?string {
    if m.slots[i].state != slotLive {
        return nil
    }
    return m.slots[i].key
}

func (m *Map[V]) ValueAt(i u64) V {
    return m.slots[i].value
}
