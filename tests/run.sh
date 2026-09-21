#!/bin/sh
# Each test declares its expectation in a leading comment:
#   // expect: <exit status>        must compile, run, and exit with that code
#   // expect-output: <line>        stdout must contain that line
#   // expect-error: <substring>    must fail to compile with that message
#
# Compiler output goes through printf rather than echo: /bin/sh here reads
# backslash escapes in echo's argument, so a diagnostic that mentions '\x1b'
# arrived at grep with an ESC in it and matched nothing.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

for src in "$root"/tests/*.sword; do
    name=$(basename "$src")
    want_exit=$(sed -n 's|^// expect: *||p' "$src" | head -1)
    want_error=$(sed -n 's|^// expect-error: *||p' "$src" | head -1)
    want_output=$(sed -n 's|^// expect-output: *||p' "$src" | head -1)

    if [ -n "$want_error" ]; then
        out=$("$shield" "$src" -o "$tmp/prog" 2>&1)
        if [ $? -eq 0 ]; then
            echo "FAIL $name: compiled, expected error '$want_error'"
            fail=$((fail + 1))
        elif ! printf '%s\n' "$out" | grep -qF "$want_error"; then
            echo "FAIL $name: wrong error"
            printf '%s\n' "$out" | sed 's/^/     /'
            fail=$((fail + 1))
        else
            pass=$((pass + 1))
        fi
        continue
    fi

    if ! "$shield" "$src" -o "$tmp/prog"; then
        echo "FAIL $name: compilation failed"
        fail=$((fail + 1))
        continue
    fi

    # A network test that hangs must not stall the whole run.
    output=$(perl -e 'alarm 30; exec @ARGV' "$tmp/prog" 2>/dev/null)
    got=$?
    if [ "$got" != "$want_exit" ]; then
        echo "FAIL $name: exit $got, want $want_exit"
        fail=$((fail + 1))
    elif [ -n "$want_output" ] && ! printf '%s\n' "$output" | grep -qF "$want_output"; then
        echo "FAIL $name: output '$output', want '$want_output'"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

# A file still named .sw is refused by name — alone, as the only file of a
# package, or beside .sword files — rather than skipped.
mkdir -p "$tmp/alone" "$tmp/mixed"
printf 'func main() int {\n    return 0\n}\n' > "$tmp/alone/old.sw"
printf 'func main() int {\n    return helper()\n}\n' > "$tmp/mixed/main.sword"
printf 'func helper() int {\n    return 0\n}\n' > "$tmp/mixed/old.sw"
for target in "$tmp/alone/old.sw" "$tmp/alone" "$tmp/mixed"; do
    out=$("$shield" "$target" -o "$tmp/prog" 2>&1)
    if [ $? -eq 0 ] || ! printf '%s\n' "$out" | grep -qF "old.sw' ends in .sw; Sword files end in .sword"; then
        echo "FAIL stale .sw in $(basename "$target"): $out"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

# The flags file beside the runtime archive is one command line however many
# lines it is written on. A newline left in the middle of it ends the clang
# invocation and runs the rest of the file as a command of its own, so the
# object on the second line is never linked.
mkdir -p "$tmp/flags"
cp "$shield" "$root/libsword_rt.a" "$tmp/flags/"
printf 'int sword_flags_shim(void) { return 7; }\n' > "$tmp/flags/shim.c"
cc -c "$tmp/flags/shim.c" -o "$tmp/flags/shim.o" 2>/dev/null
{ cat "$root/libsword_rt.flags"; echo "$tmp/flags/shim.o"; } \
    > "$tmp/flags/libsword_rt.flags"
cat > "$tmp/flags/main.sword" <<'EOF'
extern func sword_flags_shim() i32

func main() int {
    return int(sword_flags_shim())
}
EOF
if ! "$tmp/flags/shield" "$tmp/flags/main.sword" -o "$tmp/flags/prog" \
        > "$tmp/flags/log" 2>&1; then
    echo "FAIL flags file of two lines: compilation failed"
    sed 's/^/     /' "$tmp/flags/log"
    fail=$((fail + 1))
else
    "$tmp/flags/prog"
    got=$?
    if [ "$got" != 7 ]; then
        echo "FAIL flags file of two lines: exit $got, want 7"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
fi

# `shield test -run` keeps the tests it names and refuses a name that is not
# one: a package with a passing and a failing test tells the three apart.
mkdir -p "$tmp/picked"
cat > "$tmp/picked/picked_test.sword" <<'EOF'
import "std/testing"

func TestPasses(mut t *testing.T) !void {
    try t.Equal(1 + 1, 2)
}

func TestFails(mut t *testing.T) !void {
    try t.Equal(1 + 1, 3)
}
EOF
picked() { # expected exit, expected text, arguments
    want=$1 text=$2
    shift 2
    out=$("$shield" test "$tmp/picked" "$@" 2>&1)
    got=$?
    if [ "$got" != "$want" ] || ! printf '%s\n' "$out" | grep -qF "$text"; then
        echo "FAIL shield test $*: exit $got, want $want with '$text'"
        printf '%s\n' "$out" | sed 's/^/     /'
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}
picked 0 "ok    1 tests" -run TestPasses
picked 1 "2 tests, 1 failed" -run TestPasses -run TestFails
picked 1 "no test 'TestPass'" -run TestPass

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
