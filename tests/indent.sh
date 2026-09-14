#!/bin/sh
# The editors' indentation, checked against tests/indent/*.sw: the leading space
# comes off every line, each editor that is installed puts it back, and the
# answer has to be the file. An editor that is not here, or does not start, is
# skipped and says so rather than passing quietly.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

failed=0

# Nothing here should wait for a key, but an editor that did would hold up the
# whole suite. An alarm inherited through exec is not enough: Emacs takes SIGALRM
# for its own timers and carries on, so the minute is kept from outside.
capped() {
    "$@" &
    pid=$!
    perl -e 'sleep 60; kill 9, $ARGV[0]' "$pid" >/dev/null 2>&1 &
    watchdog=$!
    wait "$pid"
    status=$?
    { kill "$watchdog"; wait "$watchdog"; } 2>/dev/null
    return $status
}

starts() {
    command -v "$1" >/dev/null 2>&1 || return 1
    case $1 in
    vim)   capped vim -Nes -u NONE -i NONE -c 'quit!' ;;
    nvim)  capped nvim --headless -u NONE -i NONE -c 'quit!' ;;
    emacs) capped emacs -Q --batch --eval '(kill-emacs 0)' ;;
    esac </dev/null >/dev/null 2>&1
}

reindent() {
    case $1 in
    vim|nvim)
        if [ "$1" = vim ]; then flags="-Nes"; else flags="--headless"; fi
        capped "$1" $flags -u NONE -i NONE \
            --cmd "set rtp^=$root/editors/vim" \
            -c 'filetype plugin indent on' \
            -c 'set filetype=sword' \
            -c 'normal! gg=G' \
            -c "write! $3" \
            -c 'quit!' \
            "$2" </dev/null >/dev/null 2>&1
        ;;
    emacs)
        capped emacs -Q --batch -L "$root/editors/emacs" -l sword-mode \
            --eval "(progn (find-file \"$2\")
                           (sword-mode)
                           (indent-region (point-min) (point-max))
                           (write-region (point-min) (point-max) \"$3\"))" \
            </dev/null >/dev/null 2>&1
        ;;
    esac
}

# A warning from the byte compiler is a mistake somebody will meet later with
# less context, so it fails here.
compiles() {
    capped emacs -Q --batch \
        --eval "(setq byte-compile-error-on-warn t
                      byte-compile-dest-file-function (lambda (_) \"$tmp/sword-mode.elc\"))" \
        -f batch-byte-compile "$root/editors/emacs/sword-mode.el" >"$tmp/compile.out" 2>&1
}

for editor in vim nvim emacs; do
    printf 'indent %-17s ' "$editor"
    if ! starts "$editor"; then
        echo "skipped: not installed or does not start"
        continue
    fi
    if [ "$editor" = emacs ] && ! compiles; then
        echo "FAIL: sword-mode.el does not byte-compile cleanly"
        sed 's/^/     /' "$tmp/compile.out"
        failed=$((failed + 1))
        continue
    fi
    bad=""
    : > "$tmp/diffs"
    for want in "$root"/tests/indent/*.sw; do
        name=$(basename "$want")
        sed 's/^[ 	]*//' "$want" > "$tmp/$name"
        rm -f "$tmp/got.sw"
        reindent "$editor" "$tmp/$name" "$tmp/got.sw"
        if ! diff -u "$want" "$tmp/got.sw" >> "$tmp/diffs" 2>&1; then
            bad="$bad $name"
        fi
    done
    if [ -n "$bad" ]; then
        echo "FAIL:$bad"
        sed 's/^/     /' "$tmp/diffs"
        failed=$((failed + 1))
    else
        echo "ok"
    fi
done

[ "$failed" -eq 0 ]
