#!/bin/sh
# Runs the standard library's own tests, which are written in Sword with
# std/testing. tests/run.sh next door tests the compiler; this tests the
# library.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"

failed=0
found=0
for test_file in "$root"/std/*/*_test.sword; do
    [ -e "$test_file" ] || continue
    pkg=$(dirname "$test_file")
    found=$((found + 1))
    printf '%-24s ' "$(basename "$pkg")"
    if ! "$shield" test "$pkg"; then
        failed=$((failed + 1))
    fi
done

if [ "$found" -eq 0 ]; then
    echo "no *_test.sword files under std/"
    exit 1
fi
[ "$failed" -eq 0 ]
