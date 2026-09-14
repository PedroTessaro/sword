package strings

import "std/testing"

// Inside the package, so the names are the ones the package declares rather
// than `strings.TrimSpace`.

func TestEqual(mut t *testing.T) !void {
    try t.Equal(Equal("ada", "ada"), true)
    try t.Equal(Equal("ada", "grace"), false)
    try t.Equal(Equal("", ""), true)
    try t.Equal(Equal("a", "ab"), false)
}

func TestTrimSpace(mut t *testing.T) !void {
    try t.Equal(TrimSpace("  ada  "), "ada")
    try t.Equal(TrimSpace("\t\nada\r\n"), "ada")
    try t.Equal(TrimSpace("ada"), "ada")
    try t.Equal(TrimSpace("   "), "")
    try t.Equal(TrimSpace(""), "")
}

func TestPrefixAndIndex(mut t *testing.T) !void {
    try t.Equal(HasPrefix("grace hopper", "grace"), true)
    try t.Equal(HasPrefix("grace", "grace hopper"), false)
    try t.Equal(IndexByte("abc", 98), 1)
    // Not found is the length, which is what keeps the parsing loops readable.
    try t.Equal(IndexByte("abc", 122), 3)
    try t.Equal(Index("hello world", "world"), 6)
    try t.Equal(Index("hello", "xyz"), 5)
    try t.Equal(Index("hello", ""), 0)
    try t.Equal(Contains("hello", "ell"), true)
}

func TestEqualFold(mut t *testing.T) !void {
    try t.Equal(EqualFold("Content-Type", "content-type"), true)
    try t.Equal(EqualFold("ada", "grace"), false)
    try t.Equal(ToLower(65), 97)
    try t.Equal(ToLower(97), 97)
}

func TestParseU64(mut t *testing.T) !void {
    try t.Equal(try ParseU64("0"), 0)
    try t.Equal(try ParseU64("4095"), 4095)

    // One input per subtest, so a failure says which one. The input travels in
    // `t.Arg`, because a subtest body cannot capture anything.
    bad := [4]string{"", "12a", "-1", " 7"}
    for s in bad {
        try t.RunWith(s, rejects, s)
    }
}

func rejects(mut t *testing.T) !void {
    ParseU64(t.Arg.Text) catch {
        return
    }
    try t.Failf("accepted it")
}

// A table with more than one column per row does not fit in `t.Arg`, so the
// loop stays in the test and names the case itself.
struct trimCase {
    input string
    want  string
}

func TestTrimSpaceTable(mut t *testing.T) !void {
    cases := [4]trimCase{
        trimCase{input: "  ada  ", want: "ada"},
        trimCase{input: "\t\nada\r\n", want: "ada"},
        trimCase{input: "   ", want: ""},
        trimCase{input: "ada", want: "ada"},
    }
    for c in cases {
        got := TrimSpace(c.input)
        if got != c.want {
            try t.Failf("{}: got {} want {}", c.input, got, c.want)
        }
    }
}
