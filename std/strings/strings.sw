package strings

error NotANumber = "that is not a number"

// Byte-oriented. Sword strings are UTF-8 but these functions work on bytes,
// which is what a protocol parser wants.

// `==` on strings compares content, so this is a thin wrapper now. It stays
// because reading `strings.Equal(a, b)` inside a parser says what is meant.
func Equal(a string, b string) bool {
    return a == b
}

func HasPrefix(s string, prefix string) bool {
    if prefix.len > s.len {
        return false
    }
    return Equal(s[0..prefix.len], prefix)
}

// Position of the first `c`, or the length of `s` when it is not there. A
// sentinel rather than an optional keeps the parsing loops readable.
func HasSuffix(s string, suffix string) bool {
    if suffix.len > s.len {
        return false
    }
    return Equal(s[s.len-suffix.len..s.len], suffix)
}

func IndexByte(s string, c u8) u64 {
    for i in 0..s.len {
        if s[i] == c {
            return i
        }
    }
    return s.len
}

// The length of the haystack when the byte is not there, the same as IndexByte,
// so the result reads as "where it is or nowhere".
func LastIndexByte(s string, c u8) u64 {
    mut at := s.len
    for at > 0 {
        at -= 1
        if s[at] == c {
            return at
        }
    }
    return s.len
}

func Index(s string, needle string) u64 {
    if needle.len == 0 {
        return 0
    }
    if needle.len > s.len {
        return s.len
    }
    last := s.len - needle.len
    for i in 0..last + 1 {
        if Equal(s[i..i+needle.len], needle) {
            return i
        }
    }
    return s.len
}

func Contains(s string, needle string) bool {
    return Index(s, needle) != s.len
}

func isSpace(c u8) bool {
    switch c {
    case 32, 9, 13, 10:
        return true
    default:
        return false
    }
}

func TrimSpace(s string) string {
    mut start u64 = 0
    for start < s.len && isSpace(s[start]) {
        start += 1
    }
    mut stop := s.len
    for stop > start && isSpace(s[stop - 1]) {
        stop -= 1
    }
    return s[start..stop]
}

func ToLower(c u8) u8 {
    if c >= 65 && c <= 90 {
        return c + 32
    }
    return c
}

// Case-insensitive comparison, which is what HTTP header names need.
func EqualFold(a string, b string) bool {
    if a.len != b.len {
        return false
    }
    for i in 0..a.len {
        if ToLower(a[i]) != ToLower(b[i]) {
            return false
        }
    }
    return true
}

func ParseU64(s string) !u64 {
    if s.len == 0 {
        return error.NotANumber
    }
    mut value u64 = 0
    for i in 0..s.len {
        c := s[i]
        if c < 48 || c > 57 {
            return error.NotANumber
        }
        value = value * 10 + u64(c - 48)
    }
    return value
}
