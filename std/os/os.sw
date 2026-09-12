package os

// The process and what it was started with. A C function cannot hand back a
// Sword string by value, so these come across as a pointer and a length and
// get stitched together here.
extern func sword_os_argc() u64
extern func sword_os_arg(i u64, out_len *u64) ?[*]u8
extern func sword_os_env(name [*]u8, name_len u64, out_len *u64) ?[*]u8
extern func sword_os_exit(code i32)
extern func sword_os_catch(sig i32) i32
extern func sword_os_wait_signal() i32
extern func sword_os_max_files() i64
extern func sword_os_raise_max_files(want i64) i64
extern func sword_os_pid() i64
extern func sword_os_kill(pid i64, sig i32) i32
extern func sword_os_hostname(out_len *u64) ?[*]u8
extern func sword_os_cpus() i64

// The ones a server cares about. A handler may do almost nothing safely, so
// these arrive down a pipe and are waited for like anything else — without
// holding a thread.
enum Signal i32 {
    Hangup    = 1
    Interrupt = 2
    Quit      = 3
    Terminate = 15
}

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

// Starts catching a signal instead of letting it do what it would. Until this
// is called the default stands, so a program that asks for nothing behaves as
// it always did.
func Catch(sig Signal) !void {
    if sword_os_catch(i32(sig)) < 0 {
        return error.CannotCatch
    }
}

// The next signal that was asked for. Waiting here costs a stack rather than a
// thread, so this is an ordinary task:
//
//     func shutdown(s *http.Server) !void {
//         try os.Catch(os.Signal.Terminate)
//         try os.Catch(os.Signal.Interrupt)
//         sig := try os.WaitSignal()
//         s.Close()          // stops accepting; the scope drains the rest
//     }
func WaitSignal() !Signal {
    got := sword_os_wait_signal()
    if got < 0 {
        return error.NoSignals
    }
    return Signal(got)
}

// How many descriptors this process may open. A server that has not raised it
// stops at whatever the shell handed it, which on a Mac is 256 — well under
// what it can otherwise hold.
// A very large answer means there is no limit.
func MaxFiles() u64 {
    n := sword_os_max_files()
    if n < 0 {
        return 0
    }
    return u64(n)
}

// Raises it as far as the hard limit allows, and answers what it ended up as.
// Zero asks for the hard limit itself.
func RaiseMaxFiles(want u64) !u64 {
    n := sword_os_raise_max_files(i64(want))
    if n < 0 {
        return error.CannotRaise
    }
    return u64(n)
}

func Pid() u64 {
    return u64(sword_os_pid())
}

// Sends a signal to a process, its own included. `os.Kill(os.Pid(), ...)` is
// how a program asks itself to shut down the same way an operator would.
func Kill(pid u64, sig Signal) !void {
    if sword_os_kill(i64(pid), i32(sig)) < 0 {
        return error.CannotSignal
    }
}

// How many cores the scheduler saw, for sizing a pool of anything else.
func Cpus() u64 {
    return u64(sword_os_cpus())
}

func Hostname() ?string {
    mut n u64 = 0
    p := sword_os_hostname(&n) orelse return nil
    return string(p[0..n])
}
