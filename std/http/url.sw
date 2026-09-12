package http

import "std/strings"

// The request target as it arrives on the request line, split at `?`. Both
// halves point into the read buffer, so nothing is copied.
struct Target {
    Path     string
    RawQuery string
}

func ParseTarget(target string) Target {
    cut := strings.IndexByte(target, 63) // '?'
    if cut == target.len {
        return Target{Path: target, RawQuery: ""}
    }
    return Target{Path: target[0..cut], RawQuery: target[cut+1..target.len]}
}

// The value of the first `name=` in a query string, still percent-encoded and
// empty when the key is not there. Run it through Unescape if the value can
// carry spaces or punctuation.
func QueryValue(query string, name string) string {
    mut rest := query
    for rest.len > 0 {
        stop := strings.IndexByte(rest, 38) // '&'
        pair := rest[0..stop]
        if stop == rest.len {
            rest = rest[rest.len..rest.len]
        } else {
            rest = rest[stop+1..rest.len]
        }
        eq := strings.IndexByte(pair, 61) // '='
        if eq == pair.len {
            continue
        }
        if pair[0..eq] == name {
            return pair[eq+1..pair.len]
        }
    }
    return ""
}

func hexDigit(c u8) !u8 {
    if c >= 48 && c <= 57 {
        return c - 48
    }
    if c >= 97 && c <= 102 {
        return c - 97 + 10
    }
    if c >= 65 && c <= 70 {
        return c - 65 + 10
    }
    return error.BadEscape
}

func hexLetter(v u8) u8 {
    if v < 10 {
        return 48 + v
    }
    return 55 + v // 'A' is 65, so 65 - 10
}

// Percent-decodes into space the caller supplies and returns the part that was
// used. A `+` becomes a space, which is how a form-encoded query writes one.
func Unescape(s string, mut into []u8) !string {
    mut n u64 = 0
    mut i u64 = 0
    for i < s.len {
        if n >= into.len {
            return error.NoSpace
        }
        c := s[i]
        if c == 37 { // '%'
            if i + 2 >= s.len {
                return error.BadEscape
            }
            high := try hexDigit(s[i+1])
            low := try hexDigit(s[i+2])
            into[n] = high * 16 + low
            i += 3
        } else if c == 43 { // '+'
            into[n] = 32
            i += 1
        } else {
            into[n] = c
            i += 1
        }
        n += 1
    }
    return string(into[0..n])
}

func unreserved(c u8) bool {
    if c >= 48 && c <= 57 {
        return true
    }
    if c >= 65 && c <= 90 || c >= 97 && c <= 122 {
        return true
    }
    return c == 45 || c == 46 || c == 95 || c == 126 // - . _ ~
}

// The other direction, for building a request target out of values that may
// contain anything.
func Escape(s string, mut into []u8) !string {
    mut n u64 = 0
    for i in 0..s.len {
        c := s[i]
        if unreserved(c) {
            if n >= into.len {
                return error.NoSpace
            }
            into[n] = c
            n += 1
            continue
        }
        if n + 3 > into.len {
            return error.NoSpace
        }
        into[n] = 37 // '%'
        into[n+1] = hexLetter(c / 16)
        into[n+2] = hexLetter(c % 16)
        n += 3
    }
    return string(into[0..n])
}
