#!/bin/sh
# Runs the standard library's own tests, which are written in Sword with
# std/testing. tests/run.sh next door tests the compiler; this tests the
# library.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"

failed=0
found=0
# A package, not a file: one with several test files is still tested once.
for pkg in "$root"/std/*/; do
    pkg=${pkg%/}
    set -- "$pkg"/*_test.sword
    [ -e "$1" ] || continue
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
