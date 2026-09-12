package fs

import "std/mem"

// Files. Unlike a socket, a file is never "not ready yet" — the wait is the
// disk, and no poller has anything to say about it. So a read here really does
// stop the thread, and the scheduler hires a replacement while it is gone.
extern func sword_fs_open(path [*]u8, path_len i64, mode i32) i32
extern func sword_fs_read(fd i32, buf [*]u8, len i64) i64
extern func sword_fs_write(fd i32, buf [*]u8, len i64) i64
extern func sword_fs_close(fd i32) i32
extern func sword_fs_size(path [*]u8, path_len i64) i64
extern func sword_fs_remove(path [*]u8, path_len i64) i32

enum Mode i32 {
    Read
    // Write truncates what is there, or creates it.
    Write
    Append
}

// The three the process is started with. Reading from Stdin and writing to the
// other two needs no opening.
const Stdin = 0
const Stdout = 1
const Stderr = 2

// A file is its descriptor. Copying one copies the number, not the file, so
// close it only where it is owned.
struct File {
    fd i32
}

func Open(path string, mode Mode) !File {
    fd := sword_fs_open(path.ptr, i64(path.len), i32(mode))
    if fd < 0 {
        return error.CannotOpen
    }
    return File{fd: fd}
}

// Wraps a descriptor the process already has, which is how stdin is read.
func Of(fd i32) File {
    return File{fd: fd}
}

// How many bytes landed in `into`; zero means the end of the file.
func (f *File) Read(mut into []u8) !u64 {
    n := sword_fs_read(f.fd, into.ptr, i64(into.len))
    if n < 0 {
        return error.ReadFailed
    }
    return u64(n)
}

func (f *File) Write(from []u8) !void {
    if from.len == 0 {
        return
    }
    if sword_fs_write(f.fd, from.ptr, i64(from.len)) < 0 {
        return error.WriteFailed
    }
}

func (f *File) WriteString(s string) !void {
    try f.Write([]u8(s))
}

func (f *File) Close() {
    sword_fs_close(f.fd)
}

// Nil when there is nothing there, or when it is a directory.
func Size(path string) ?u64 {
    n := sword_fs_size(path.ptr, i64(path.len))
    if n < 0 {
        return nil
    }
    return u64(n)
}

func Exists(path string) bool {
    return Size(path) != nil
}

func Remove(path string) !void {
    if sword_fs_remove(path.ptr, i64(path.len)) < 0 {
        return error.CannotRemove
    }
}

// The whole file, in memory the caller's allocator gives. The size is asked for
// first and the read is checked against it, so a file that grows between the
// two does not overrun anything.
func ReadAll(path string, mut a mem.Allocator) ![]u8 {
    size := Size(path) orelse return error.CannotOpen
    mut f := try Open(path, Mode.Read)
    defer f.Close()

    mut room := mem.Alloc[u8](a, size) orelse return error.OutOfMemory
    mut have u64 = 0
    for have < size {
        n := try f.Read(room[have..size])
        if n == 0 {
            break
        }
        have += n
    }
    return room[0..have]
}

func WriteAll(path string, data []u8) !void {
    mut f := try Open(path, Mode.Write)
    defer f.Close()
    try f.Write(data)
}

// Everything waiting on standard input, which is how a program is piped into.
// It grows as it reads, so the caller's allocator has to be one that can.
func ReadStdin(mut a mem.Allocator, most u64) ![]u8 {
    mut f := Of(Stdin)
    mut room := mem.Alloc[u8](a, most) orelse return error.OutOfMemory
    mut have u64 = 0
    for have < most {
        n := try f.Read(room[have..most])
        if n == 0 {
            break
        }
        have += n
    }
    return room[0..have]
}
