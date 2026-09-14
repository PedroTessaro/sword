// expect: 42
// expect-output: released memory reads as released
// The language stops a task from outliving the memory it borrowed, and stops a
// function from handing back its own frame. It does not stop you from freeing
// something and reading it afterwards — nothing here tracks that — so what it
// offers instead is a way to find out loudly, on purpose, where you are looking.

import "std/io"
import "std/mem"

func main() !int {
    // An allocator that writes over what goes back through it.
    mut sys := mem.NewSystem()
    mut watched := mem.NewWatched(&sys)
    mut xs := mem.Alloc[u64](&watched, 4) orelse return 1
    xs[0] = 42
    xs[3] = 42
    mem.Free(&watched, xs)
    if watched.Written != 32 {
        return 2
    }

    // An arena that does the same when it is reset. This is the one that matters
    // in a server: a per-request arena is reset between requests, and anything
    // still pointing into it is looking at the next request.
    mut room := [4096]u8{}
    mut arena := mem.NewArena(room[..])
    arena.Watch = true

    mut before := mem.Alloc[u64](&arena, 2) orelse return 3
    before[0] = 7
    before[1] = 9
    arena.Reset()
    // Deliberately wrong, which is the point: every byte is 0xDE now.
    if before[0] != 16059518370053021406 || before[1] != before[0] {
        return 4
    }

    // And the arena still works afterwards, poisoned or not.
    mut after := mem.Alloc[u64](&arena, 2) orelse return 5
    after[0] = 1
    after[1] = 2
    if after[0] + after[1] != 3 {
        return 6
    }

    // Off by default, because a write per byte is not free.
    mut plain := mem.NewArena(room[..])
    if plain.Watch {
        return 7
    }

    try io.Print("released memory reads as released\n")
    return 42
}
