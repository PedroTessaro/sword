package time

extern func sword_time_mono() i64
extern func sword_time_unix() i64
extern func sword_time_sleep(nanos i64)

const nanosPerMicro = 1000
const nanosPerMilli = 1000000
const nanosPerSecond = 1000000000

// A length of time, always in nanoseconds underneath. Naming it stops the
// usual question of whether a bare number meant seconds or milliseconds.
struct Duration {
    ns i64
}

// A reading of a clock that only moves forward. Two of them subtract into a
// Duration; on its own the number means nothing, because the clock counts
// from an arbitrary point.
struct Instant {
    ns i64
}

func Nanos(n i64) Duration {
    return Duration{ns: n}
}

func Micros(n i64) Duration {
    return Duration{ns: n * nanosPerMicro}
}

func Millis(n i64) Duration {
    return Duration{ns: n * nanosPerMilli}
}

func Seconds(n i64) Duration {
    return Duration{ns: n * nanosPerSecond}
}

func (d Duration) AsNanos() i64 {
    return d.ns
}

func (d Duration) AsMicros() i64 {
    return d.ns / nanosPerMicro
}

func (d Duration) AsMillis() i64 {
    return d.ns / nanosPerMilli
}

func (d Duration) AsSeconds() f64 {
    return f64(d.ns) / f64(nanosPerSecond)
}

func (d Duration) Add(other Duration) Duration {
    return Duration{ns: d.ns + other.ns}
}

func (d Duration) Less(other Duration) bool {
    return d.ns < other.ns
}

// The monotonic clock. Use it to measure how long something took; a system
// time adjustment cannot drag it backwards mid-measurement.
func Now() Instant {
    return Instant{ns: sword_time_mono()}
}

func Since(start Instant) Duration {
    return Duration{ns: sword_time_mono() - start.ns}
}

func (t Instant) Add(d Duration) Instant {
    return Instant{ns: t.ns + d.ns}
}

// The reading itself, for handing to something that wants an absolute point —
// a connection deadline, say. On its own the number means nothing.
func (t Instant) AsNanos() i64 {
    return t.ns
}

// Negative when `t` is in the past, which is how a deadline is checked.
func (t Instant) Until() Duration {
    return Duration{ns: t.ns - sword_time_mono()}
}

// Seconds since the Unix epoch. This is the wall clock, so it is the one to
// stamp a record with and the wrong one to measure with.
func Unix() i64 {
    return sword_time_unix() / nanosPerSecond
}

func UnixNanos() i64 {
    return sword_time_unix()
}

// Parks the task for at least this long. The scheduler is told first, so the
// thread it was using goes to other work instead of sitting idle.
func Sleep(d Duration) {
    sword_time_sleep(d.ns)
}
