package os

// The process and what it was started with. A C function cannot hand back a
// Sword string by value, so these come across as a pointer and a length and
// get stitched together here.
extern func sword_os_argc() u64
extern func sword_os_arg(i u64, out_len *u64) ?[*]u8
extern func sword_os_env(name [*]u8, name_len u64, out_len *u64) ?[*]u8
extern func sword_os_exit(code i32)

// Including the program's own name, the way argc does. So a program run with
// no arguments still reports one.
func Argc() u64 {
    return sword_os_argc()
}

// Empty when `i` is past the end, which keeps a missing argument from turning
// into an error the caller has to unwrap.
func Arg(i u64) string {
    mut n u64 = 0
    p := sword_os_arg(i, &n) orelse return ""
    return string(p[0..n])
}

// Fills a caller-provided array rather than allocating, and returns the part
// of it that was used:
//
//     mut room := [8]string{}
//     args := os.Args(room[..])
//
// Arguments past the end of `into` are dropped; ask Argc first if that
// matters.
func Args(mut into []string) []string {
    mut count := Argc()
    if count > into.len {
        count = into.len
    }
    for i in 0..count {
        into[i] = Arg(i)
    }
    return into[0..count]
}

// Nil when the variable is not set. A variable set to nothing is set, and
// comes back as the empty string.
func Env(name string) ?string {
    mut n u64 = 0
    p := sword_os_env(name.ptr, name.len, &n) orelse return nil
    return string(p[0..n])
}

// What Env gives back when the variable is missing, for the common case where
// a default is more useful than a branch.
func EnvOr(name string, fallback string) string {
    return Env(name) orelse fallback
}

// Ends the process immediately. Deferred statements do not run and buffered
// output is not flushed, so prefer returning from main where you can.
func Exit(code i32) {
    sword_os_exit(code)
}
