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

for src in "$root"/tests/*.sw; do
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

    output=$("$tmp/prog" 2>/dev/null)
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

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
