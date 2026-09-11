package fmt

import "std/bytes"

// Sword has no variadic functions, so there is no Sprintf here. Formatting is
// a sequence of writes into a buffer instead, which costs a line or two more
// and never has a format string that disagrees with its arguments.

const decimalZero = 48
const minus = 45

func U64(mut b *bytes.Buffer, v u64) !void {
    try b.WriteU64(v)
}

func I64(mut b *bytes.Buffer, v i64) !void {
    if v < 0 {
        try b.WriteByte(minus)
        // Negating the smallest value would overflow, so the magnitude is
        // taken in unsigned arithmetic.
        try b.WriteU64(u64(0) -% u64(v))
        return
    }
    try b.WriteU64(u64(v))
}

func Bool(mut b *bytes.Buffer, v bool) !void {
    if v {
        try b.WriteString("true")
        return
    }
    try b.WriteString("false")
}

func Hex(mut b *bytes.Buffer, v u64, width u64) !void {
    mut pad := width
    mut digits := [16]u8{}
    mut n := v
    mut used u64 = 0
    for {
        d := u8(n & 15)
        if d < 10 {
            digits[used] = decimalZero + d
        } else {
            digits[used] = 87 + d
        }
        used += 1
        n >>= 4
        if n == 0 {
            break
        }
    }
    for used < pad {
        try b.WriteByte(decimalZero)
        pad -= 1
    }
    for k in 0..used {
        try b.WriteByte(digits[used - 1 - k])
    }
}

// Fixed point with `decimals` places. Enough for logs and JSON numbers; not a
// replacement for a proper shortest-round-trip printer.
func F64(mut b *bytes.Buffer, v f64, decimals u64) !void {
    mut x := v
    if x < 0.0 {
        try b.WriteByte(minus)
        x = -x
    }

    mut scale f64 = 1.0
    for i in 0..decimals {
        scale *= 10.0
    }
    scaled := u64(x * scale + 0.5)
    whole := scaled / u64(scale)
    frac := scaled - whole * u64(scale)

    try b.WriteU64(whole)
    if decimals == 0 {
        return
    }
    try b.WriteByte(46)

    // Leading zeros of the fraction are significant.
    mut limit := u64(scale) / 10
    for limit > 0 {
        if frac >= limit {
            break
        }
        try b.WriteByte(decimalZero)
        limit /= 10
    }
    if frac > 0 {
        try b.WriteU64(frac)
    }
}

func Pad(mut b *bytes.Buffer, s string, width u64) !void {
    try b.WriteString(s)
    mut at := s.len
    for at < width {
        try b.WriteByte(32)
        at += 1
    }
}

// Escapes for a JSON string, quotes included. Control characters go out as
// \u00XX, which is the only form every parser accepts.
func Quote(mut b *bytes.Buffer, s string) !void {
    try b.WriteByte(34)
    for i in 0..s.len {
        c := s[i]
        if c == 34 || c == 92 {
            try b.WriteByte(92)
            try b.WriteByte(c)
        } else if c == 10 {
            try b.WriteString("\\n")
        } else if c == 13 {
            try b.WriteString("\\r")
        } else if c == 9 {
            try b.WriteString("\\t")
        } else if c < 32 {
            try b.WriteString("\\u00")
            try Hex(b, u64(c), 2)
        } else {
            try b.WriteByte(c)
        }
    }
    try b.WriteByte(34)
}
