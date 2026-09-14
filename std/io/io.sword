package io

import "std/fmt"

error WriteFailed = "the write failed"

extern func write(fd i32, buf [*]u8, n u64) i64

const Stdout = 1
const Stderr = 2
const bufferSize = 1024

// Buffered output over a fixed block of stack. Printing must not need an
// allocator: it is the first thing anybody writes, and asking for an arena to
// print a number would be absurd.
struct Writer {
    fd    i32
    buf   [bufferSize]u8
    count u64
}

func NewWriter(fd i32) Writer {
    return Writer{fd: fd, buf: [bufferSize]u8{}, count: 0}
}

func (mut w *Writer) Flush() !void {
    if w.count == 0 {
        return
    }
    n := write(w.fd, w.buf[0..w.count].ptr, w.count)
    w.count = 0
    if n < 0 {
        return error.WriteFailed
    }
}

func (mut w *Writer) WriteByte(c u8) !void {
    if w.count == bufferSize {
        try w.Flush()
    }
    w.buf[w.count] = c
    w.count += 1
}

func (mut w *Writer) Write(p []u8) !void {
    for i in 0..p.len {
        try w.WriteByte(p[i])
    }
}

func (mut w *Writer) WriteString(s string) !void {
    try w.Write([]u8(s))
}

func Write(fd i32, s string) !u64 {
    n := write(fd, s.ptr, s.len)
    if n < 0 {
        return error.WriteFailed
    }
    return u64(n)
}

func Print(s string) !void {
    try Write(Stdout, s)
}

func Fail(s string) !void {
    try Write(Stderr, s)
}

// `{}` takes the next argument, whatever its type.
func Fprintf(fd i32, format string, args ...any) !void {
    mut w := NewWriter(fd)
    try fmt.Format(&w, format, args)
    try w.Flush()
}

func Printf(format string, args ...any) !void {
    try Fprintf(Stdout, format, args...)
}

func Errorf(format string, args ...any) !void {
    try Fprintf(Stderr, format, args...)
}

// A line of values separated by spaces, the quick way to look at something.
func Println(args ...any) !void {
    mut w := NewWriter(Stdout)
    for i in 0..args.len {
        if i > 0 {
            try w.WriteByte(32)
        }
        try fmt.Value(&w, args[i])
    }
    try w.WriteByte(10)
    try w.Flush()
}
