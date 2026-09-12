// expect: 42
// expect-output: files work
// Reading and writing, and forty tasks reading at once. A file is never "not
// ready yet" — the wait is the disk — so file work really does stop a thread
// and the pool grows to cover it. That is the honest cost, and it shows up in
// the thread count.

import "std/fs"
import "std/io"
import "std/mem"

const path = "/tmp/sword_files_test.txt"
const content = "hello from sword\n"

func reader(where string, mut done *atomic[u64]) !void {
    mut backing := [8192]u8{}
    mut arena := mem.NewArena(backing[..])
    body := fs.ReadAll(where, &arena) catch return
    if string(body) == content {
        done.Add(1)
    }
}

func main() !int {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])

    try fs.WriteAll(path, []u8(content))
    if !fs.Exists(path) {
        return 1
    }
    if (fs.Size(path) orelse 0) != 17 {
        return 2
    }

    back := try fs.ReadAll(path, &arena)
    if string(back) != content {
        return 3
    }

    // Append leaves what was there.
    mut f := try fs.Open(path, fs.Mode.Append)
    try f.WriteString("and again\n")
    f.Close()
    if (fs.Size(path) orelse 0) != 27 {
        return 4
    }

    // Write truncates it.
    try fs.WriteAll(path, []u8(content))
    if (fs.Size(path) orelse 0) != 17 {
        return 5
    }

    mut done := atomic[u64](0)
    scope {
        for i in 0..40 {
            spawn reader(path, &done)
        }
    }
    if done.Load() != 40 {
        return 6
    }

    try fs.Remove(path)
    if fs.Exists(path) {
        return 7
    }
    // Nothing there and a directory both come back as absent rather than as a
    // number that would be a lie.
    if fs.Size("/definitely/not/here") != nil {
        return 8
    }
    if fs.Size("/tmp") != nil {
        return 9
    }
    // Opening what is not there fails rather than handing back a file.
    mut refused := false
    fs.Open("/definitely/not/here", fs.Mode.Read) catch {
        refused = true
    }
    if !refused {
        return 10
    }

    try io.Print("files work\n")
    return 42
}
