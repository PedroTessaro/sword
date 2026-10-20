#!/bin/sh
# docs/grammar.md against the parser: see tests/grammar.py.
exec python3 "$(dirname "$0")/grammar.py"
