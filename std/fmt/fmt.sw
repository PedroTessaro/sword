package fmt

import "std/bytes"
import "std/mem"

// Formatting writes into a sink rather than into one particular buffer, so the
// same code serves a growable buffer, a socket, or a fixed block of stack.
// std/bytes.Buffer satisfies this without knowing about it.
interface Sink {
    WriteByte(c u8) !void
    Write(p []u8) !void
}

// The tags an `any` carries. These belong to the language; the names are here
// so that reading one does not mean remembering numbers.
const KindNone = 0
const KindBool = 1
const KindInt = 2
const KindUint = 3
const KindFloat = 4
const KindString = 5
const KindPointer = 6

const digitZero = 48
const minus = 45

func Str(mut out Sink, s string) !void {
    try out.Write([]u8(s))
}

func U64(mut out Sink, v u64) !void {
    if v == 0 {
        try out.WriteByte(digitZero)
        return
    }
    // Written backwards into a scratch array, then reversed: the length is not
    // known until the digits run out.
    mut digits := [20]u8{}
    mut n := v
    mut used u64 = 0
    for n > 0 {
        digits[used] = u8(digitZero + n % 10)
        n /= 10
        used += 1
    }
    for k in 0..used {
        try out.WriteByte(digits[used - 1 - k])
    }
}

func I64(mut out Sink, v i64) !void {
    if v < 0 {
        try out.WriteByte(minus)
        // Negating the smallest value would overflow, so the magnitude is
        // taken in unsigned arithmetic.
        try U64(out, u64(0) -% u64(v))
        return
    }
    try U64(out, u64(v))
}

func Bool(mut out Sink, v bool) !void {
    if v {
        try Str(out, "true")
        return
    }
    try Str(out, "false")
}

func Hex(mut out Sink, v u64, width u64) !void {
    mut digits := [16]u8{}
    mut n := v
    mut used u64 = 0
    for {
        d := u8(n & 15)
        if d < 10 {
            digits[used] = digitZero + d
        } else {
            digits[used] = 87 + d
        }
        used += 1
        n >>= 4
        if n == 0 {
            break
        }
    }
    mut pad := width
    for used < pad {
        try out.WriteByte(digitZero)
        pad -= 1
    }
    for k in 0..used {
        try out.WriteByte(digits[used - 1 - k])
    }
}

// Fixed point with `decimals` places. Enough for logs and JSON numbers; not a
// shortest-round-trip printer.
func F64(mut out Sink, v f64, decimals u64) !void {
    mut x := v
    if x < 0.0 {
        try out.WriteByte(minus)
        x = -x
    }

    mut scale f64 = 1.0
    for i in 0..decimals {
        scale *= 10.0
    }
    scaled := u64(x * scale + 0.5)
    unit := u64(scale)
    whole := scaled / unit
    frac := scaled - whole * unit

    try U64(out, whole)
    if decimals == 0 {
        return
    }
    try out.WriteByte(46)

    // Leading zeros of the fraction are significant.
    mut limit := unit / 10
    for limit > 0 {
        if frac >= limit {
            break
        }
        try out.WriteByte(digitZero)
        limit /= 10
    }
    if frac > 0 {
        try U64(out, frac)
    }
}

// The default for a float: up to six places, with trailing zeros dropped so
// that 1.5 does not print as 1.500000.
func Float(mut out Sink, v f64) !void {
    mut x := v
    if x < 0.0 {
        try out.WriteByte(minus)
        x = -x
    }
    scaled := u64(x * 1000000.0 + 0.5)
    whole := scaled / 1000000
    mut frac := scaled - whole * 1000000
    try U64(out, whole)
    if frac == 0 {
        try Str(out, ".0")
        return
    }
    try out.WriteByte(46)
    mut limit u64 = 100000
    for limit > 0 {
        if frac >= limit {
            break
        }
        try out.WriteByte(digitZero)
        limit /= 10
    }
    for frac % 10 == 0 {
        frac /= 10
    }
    try U64(out, frac)
}

func Pad(mut out Sink, s string, width u64) !void {
    try Str(out, s)
    mut at := s.len
    for at < width {
        try out.WriteByte(32)
        at += 1
    }
}

// Escapes for a JSON string, quotes included.
func Quote(mut out Sink, s string) !void {
    try out.WriteByte(34)
    for i in 0..s.len {
        c := s[i]
        if c == 34 || c == 92 {
            try out.WriteByte(92)
            try out.WriteByte(c)
        } else if c == 10 {
            try Str(out, "\\n")
        } else if c == 13 {
            try Str(out, "\\r")
        } else if c == 9 {
            try Str(out, "\\t")
        } else if c < 32 {
            try Str(out, "\\u00")
            try Hex(out, u64(c), 2)
        } else {
            try out.WriteByte(c)
        }
    }
    try out.WriteByte(34)
}

func Value(mut out Sink, v any) !void {
    if v.Kind == KindString {
        try Str(out, v.Text)
    } else if v.Kind == KindBool {
        try Bool(out, v.Int != 0)
    } else if v.Kind == KindInt {
        try I64(out, v.Int)
    } else if v.Kind == KindUint {
        try U64(out, u64(v.Int))
    } else if v.Kind == KindFloat {
        try Float(out, v.Real)
    } else if v.Kind == KindPointer {
        try Str(out, "0x")
        try Hex(out, u64(v.Int), 1)
    } else {
        try Str(out, "<none>")
    }
}

// `{}` takes the next argument. `{x}` prints an integer in hex, `{.N}` a float
// with N places. `{{` and `}}` are literal braces.
//
// There are no type letters because there is nothing for them to do: the
// argument already knows what it is, so a format string cannot disagree with
// it.
func Format(mut out Sink, format string, args []any) !void {
    mut next u64 = 0
    mut i u64 = 0
    for i < format.len {
        c := format[i]
        if c == 125 {
            i += 1
            if i < format.len && format[i] == 125 {
                i += 1
            }
            try out.WriteByte(125)
            continue
        }
        if c != 123 {
            try out.WriteByte(c)
            i += 1
            continue
        }
        i += 1
        if i < format.len && format[i] == 123 {
            try out.WriteByte(123)
            i += 1
            continue
        }

        mut spec := ""
        start := i
        for i < format.len && format[i] != 125 {
            i += 1
        }
        spec = format[start..i]
        if i < format.len {
            i += 1
        }

        if next >= args.len {
            try Str(out, "{missing}")
            continue
        }
        arg := args[next]
        next += 1
        try formatOne(out, arg, spec)
    }
}

func formatOne(mut out Sink, arg any, spec string) !void {
    if spec.len == 0 {
        try Value(out, arg)
        return
    }
    if spec.len == 1 && spec[0] == 120 {
        try Hex(out, u64(arg.Int), 1)
        return
    }
    if spec[0] == 46 {
        mut places u64 = 0
        for i in 1..spec.len {
            c := spec[i]
            if c < 48 || c > 57 {
                try Value(out, arg)
                return
            }
            places = places * 10 + u64(c - 48)
        }
        try F64(out, arg.Real, places)
        return
    }
    try Value(out, arg)
}

// --- into an allocator -----------------------------------------------------

// Formats into memory the caller owns. `io.Printf` exists for the common case
// of writing straight out, and needs no allocator at all.
func Sprintf(mut a mem.Allocator, format string, args ...any) !string {
    mut out := try bytes.New(a, format.len + 32)
    try Format(&out, format, args...)
    return out.Str()
}
