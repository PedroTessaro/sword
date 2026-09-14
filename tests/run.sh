#!/bin/sh
# Each test declares its expectation in a leading comment:
#   // expect: <exit status>        must compile, run, and exit with that code
#   // expect-output: <line>        stdout must contain that line
#   // expect-error: <substring>    must fail to compile with that message
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
        elif ! echo "$out" | grep -qF "$want_error"; then
            echo "FAIL $name: wrong error"
            echo "$out" | sed 's/^/     /'
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
    elif [ -n "$want_output" ] && ! echo "$output" | grep -qF "$want_output"; then
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
    if [ $? -eq 0 ] || ! echo "$out" | grep -qF "old.sw' ends in .sw; Sword files end in .sword"; then
        echo "FAIL stale .sw in $(basename "$target"): $out"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

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
    if [ "$got" != "$want" ] || ! echo "$out" | grep -qF "$text"; then
        echo "FAIL shield test $*: exit $got, want $want with '$text'"
        echo "$out" | sed 's/^/     /'
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
