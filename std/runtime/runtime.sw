package runtime

// What the scheduler is doing, for whoever has to run the program rather than
// write it. Nothing here costs more than a few relaxed loads, so it is cheap
// enough to read on a timer or to answer a request with.
//
// `extern struct` because the runtime fills it in from C: the fields have to
// stay in the order they are written.
extern struct Stats {
    // Worker threads, including any hired to cover one stopped in the kernel.
    Threads i64
    // Tasks spawned and not yet picked up. A queue that keeps growing is the
    // first sign of a server taking more than it can serve.
    Queued i64
    // Threads stopped inside a syscall that cannot be put down — name
    // resolution, mostly.
    Parked i64
    // Task stacks alive, in use or waiting on a worker's pile. Each is a
    // megabyte reserved and rather less touched.
    Stacks i64
    // Tasks begun and ended since the program started.
    Started  i64
    Finished i64
}

extern func sword_runtime_stats(out *Stats)

func Read() Stats {
    mut out := Stats{Threads: 0, Queued: 0, Parked: 0, Stacks: 0, Started: 0,
                     Finished: 0}
    sword_runtime_stats(&out)
    return out
}

// Tasks that have begun and not ended: what the program is in the middle of.
func (s Stats) Running() i64 {
    return s.Started - s.Finished
}
