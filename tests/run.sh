#!/bin/sh
# Each test declares its expectation in a leading comment:
#   // expect: <exit status>        must compile, run, and exit with that code
#   // expect-output: <line>        stdout must contain that line
#   // expect-error: <substring>    must fail to compile with that message
#   // output-varies: <why>         prints something that depends on the clock
#                                   or the pool, so tests/threads.sh compares
#                                   only its exit status across thread counts
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

# --link puts an object of the caller's own on the link line. The path is
# quoted on the way to the shell, so one with a space in it is a path.
cp "$tmp/flags/shim.o" "$tmp/flags/a shim.o"
if ! "$shield" "$tmp/flags/main.sword" -o "$tmp/flags/linked" \
        --link "$tmp/flags/a shim.o" > "$tmp/flags/log" 2>&1; then
    echo "FAIL --link: compilation failed"
    sed 's/^/     /' "$tmp/flags/log"
    fail=$((fail + 1))
else
    "$tmp/flags/linked"
    got=$?
    if [ "$got" != 7 ]; then
        echo "FAIL --link: exit $got, want 7"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
fi

# An extern that ends in '...' is called the way C calls a variadic function.
# On Apple's arm64 that is the whole difference: a variadic argument travels on
# the stack while a fixed one travels in a register, so the same call declared
# with a fixed arity reaches the callee as garbage. The shim reads its
# arguments with va_arg, which is what makes the two tell apart.
mkdir -p "$tmp/varargs"
cat > "$tmp/varargs/shim.c" <<'EOF'
#include <stdarg.h>

long sword_vsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long total = 0;
  for (int i = 0; i < n; i++) total += va_arg(ap, long);
  va_end(ap);
  return total;
}

// C promotes anything narrower than an int, and a float to a double; the
// callee reads the promoted width whatever the call site passed.
int sword_vpromote(int tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int small = va_arg(ap, int);
  double wide = va_arg(ap, double);
  va_end(ap);
  return small + (int)wide;
}
EOF
cc -c "$tmp/varargs/shim.c" -o "$tmp/varargs/shim.o" 2>/dev/null
cat > "$tmp/varargs/main.sword" <<'EOF'
extern func sword_vsum(n i32, ...) i64
extern func sword_vpromote(tag i32, ...) i32

func main() int {
    if sword_vsum(3, i64(10), i64(20), i64(12)) != 42 {
        return 1
    }
    if sword_vpromote(0, u8(200), f32(1.5)) != 201 {
        return 2
    }
    return 0
}
EOF
if ! "$shield" "$tmp/varargs/main.sword" -o "$tmp/varargs/prog" \
        --link "$tmp/varargs/shim.o" > "$tmp/varargs/log" 2>&1; then
    echo "FAIL variadic extern: compilation failed"
    sed 's/^/     /' "$tmp/varargs/log"
    fail=$((fail + 1))
else
    "$tmp/varargs/prog"
    got=$?
    if [ "$got" != 0 ]; then
        echo "FAIL variadic extern: exit $got"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
fi

# An integer narrower than an int reaches C widened, the way C would have
# passed it. Apple's arm64 leaves the widening to the caller, so without it the
# callee reads -117 as whatever the register's upper bits happened to say.
mkdir -p "$tmp/narrow"
cat > "$tmp/narrow/shim.c" <<'EOF'
#include <stdbool.h>
#include <stdint.h>

int sword_widen8(int8_t x) { return x; }
unsigned sword_widen16(uint16_t x) { return x; }
int sword_truth(bool b) { return b; }
EOF
cc -O2 -c "$tmp/narrow/shim.c" -o "$tmp/narrow/shim.o" 2>/dev/null
cat > "$tmp/narrow/main.sword" <<'EOF'
extern func sword_widen8(x i8) i32
extern func sword_widen16(x u16) u32
extern func sword_truth(b bool) i32

func main() int {
    mut small i8 = 117
    small = -small
    if sword_widen8(small) != -117 {
        return 1
    }
    wide u16 = 65535
    if sword_widen16(wide) != 65535 {
        return 2
    }
    if sword_truth(small < 0) != 1 {
        return 3
    }
    return 0
}
EOF
if ! "$shield" "$tmp/narrow/main.sword" -o "$tmp/narrow/prog" \
        --link "$tmp/narrow/shim.o" > "$tmp/narrow/log" 2>&1; then
    echo "FAIL narrow extern: compilation failed"
    sed 's/^/     /' "$tmp/narrow/log"
    fail=$((fail + 1))
else
    "$tmp/narrow/prog"
    got=$?
    if [ "$got" != 0 ]; then
        echo "FAIL narrow extern: exit $got"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
fi

# A library named in the source is linked without being named on the command
# line: dynamically by default, from its archive with --static. Each way is
# told apart by taking the library away afterwards — a dynamic program then
# fails to start, a static one does not notice.
mkdir -p "$tmp/libs/lib" "$tmp/libs/onlyshared"
cat > "$tmp/libs/shim.c" <<'EOF'
int sword_lib_answer(void) { return 42; }
int sword_lib_present(void) { return 7; }
EOF
cc -c -fPIC "$tmp/libs/shim.c" -o "$tmp/libs/shim.o" 2>/dev/null
ar rcs "$tmp/libs/lib/libswordlib.a" "$tmp/libs/shim.o"
if [ "$(uname -s)" = Darwin ]; then
    shared=libswordlib.dylib
    cc -dynamiclib "$tmp/libs/shim.o" -install_name "@rpath/$shared" \
        -o "$tmp/libs/lib/$shared" 2>/dev/null
else
    shared=libswordlib.so
    cc -shared "$tmp/libs/shim.o" -o "$tmp/libs/lib/$shared" 2>/dev/null
fi
cp "$tmp/libs/lib/$shared" "$tmp/libs/onlyshared/"
cat > "$tmp/libs/main.sword" <<'EOF'
extern "swordlib" func sword_lib_answer() i32
extern weak "swordlib" func sword_lib_present() i32
extern weak func sword_lib_nowhere() i32

func main() int {
    if f := sword_lib_nowhere {
        return 1
    }
    g := sword_lib_present orelse return 2
    if g() != 7 {
        return 3
    }
    return int(sword_lib_answer())
}
EOF
libsrc="$tmp/libs/main.sword"
libcheck() { # name, exit wanted once the library is moved away, shield args
    what=$1 want_away=$2
    shift 2
    if ! "$shield" "$libsrc" -o "$tmp/libs/prog" "$@" \
            > "$tmp/libs/log" 2>&1; then
        echo "FAIL $what: compilation failed"
        sed 's/^/     /' "$tmp/libs/log"
        fail=$((fail + 1))
        return
    fi
    "$tmp/libs/prog"
    got=$?
    mv "$tmp/libs/lib" "$tmp/libs/away"
    "$tmp/libs/prog" 2>/dev/null
    away=$?
    mv "$tmp/libs/away" "$tmp/libs/lib"
    if [ "$got" != 42 ]; then
        echo "FAIL $what: exit $got, want 42"
        fail=$((fail + 1))
    elif [ "$want_away" = 42 ] && [ "$away" != 42 ]; then
        echo "FAIL $what: exit $away once the library was gone, want 42"
        fail=$((fail + 1))
    elif [ "$want_away" != 42 ] && [ "$away" = 42 ]; then
        echo "FAIL $what: still ran without the library, so it was not dynamic"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}
libcheck "extern library, dynamic" gone --link "-L$tmp/libs/lib"
libcheck "extern library, --static" 42 --static --link "-L$tmp/libs/lib"

# --static with only a shared library to be had is refused rather than quietly
# linked the other way.
out=$("$shield" "$tmp/libs/main.sword" -o "$tmp/libs/prog" --static \
      --link "-L$tmp/libs/onlyshared" 2>&1)
if [ $? -eq 0 ] || ! printf '%s\n' "$out" | grep -qF "there is no libswordlib.a"; then
    echo "FAIL --static without an archive: $out"
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi

# pkg-config is asked where a library is: under the name the source used, then
# with lib in front, which is how libz9.pc is found for `extern "z9"` here and
# turns out to mean -lswordlib. A machine without pkg-config gets a stand-in
# that knows only --libs.
mkdir -p "$tmp/libs/pc" "$tmp/libs/bin"
printf 'Name: z9\nDescription: test\nVersion: 1\nLibs: -L%s -lswordlib\n' \
    "$tmp/libs/lib" > "$tmp/libs/pc/libz9.pc"
pkgpath=$PATH
if ! command -v pkg-config > /dev/null; then
    cat > "$tmp/libs/bin/pkg-config" <<'EOF'
#!/bin/sh
for last; do :; done
[ -f "$PKG_CONFIG_PATH/$last.pc" ] || exit 1
sed -n 's/^Libs: //p' "$PKG_CONFIG_PATH/$last.pc"
EOF
    chmod +x "$tmp/libs/bin/pkg-config"
    pkgpath="$tmp/libs/bin:$PATH"
fi
sed 's/"swordlib"/"z9"/' "$tmp/libs/main.sword" > "$tmp/libs/viapc.sword"
libsrc="$tmp/libs/viapc.sword"
saved_path=$PATH
PATH=$pkgpath
PKG_CONFIG_PATH="$tmp/libs/pc"
export PKG_CONFIG_PATH
libcheck "extern library through pkg-config" gone
libcheck "extern library through pkg-config, --static" 42 --static
PATH=$saved_path
unset PKG_CONFIG_PATH
libsrc="$tmp/libs/main.sword"

# Two packages may declare the same C function, but not disagree about it.
mkdir -p "$tmp/clash/other"
cat > "$tmp/clash/main.sword" <<'EOF'
import "other"

extern func sword_lib_answer() i32

func main() int {
    return int(sword_lib_answer()) + other.Zero()
}
EOF
cat > "$tmp/clash/other/other.sword" <<'EOF'
package other

extern weak func sword_lib_answer() i32

func Zero() int {
    return 0
}
EOF
out=$("$shield" "$tmp/clash/main.sword" -o "$tmp/clash/prog" 2>&1)
if [ $? -eq 0 ] || ! printf '%s\n' "$out" | grep -qF "is weak here and not where it was first declared"; then
    echo "FAIL weak and not weak: $out"
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi

# -c stops at the object, which needs no runtime, so it is also how a program
# is built for a machine nothing here can link for: the object for x86-64
# Linux comes out the same on any machine clang targets.
mkdir -p "$tmp/cross"
printf 'func main() int {\n    return 0\n}\n' > "$tmp/cross/main.sword"
expect_object() { # what, file, the first four bytes as od prints them
    if [ ! -f "$2" ] || [ "$(od -An -tx1 -N4 "$2" | tr -d ' \n')" != "$3" ]; then
        echo "FAIL $1: no object, or not the kind asked for"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}
"$shield" "$tmp/cross/main.sword" -c -o "$tmp/cross/native.o" > /dev/null 2>&1
if [ "$(uname -s)" = Darwin ]; then
    expect_object "-c" "$tmp/cross/native.o" cffaedfe
else
    expect_object "-c" "$tmp/cross/native.o" 7f454c46
fi
"$shield" "$tmp/cross/main.sword" --target=x86_64-unknown-linux-gnu -c \
    -o "$tmp/cross/linux.o" > /dev/null 2>&1
expect_object "--target -c" "$tmp/cross/linux.o" 7f454c46

refused() { # what, the text to find, arguments
    what=$1 text=$2
    shift 2
    out=$("$shield" "$@" 2>&1)
    if [ $? -eq 0 ] || ! printf '%s\n' "$out" | grep -qF "$text"; then
        echo "FAIL $what: $out"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}
refused "--target with 16-bit pointers" "not a 64-bit architecture" \
    "$tmp/cross/main.sword" --target=avr -c
refused "--target big-endian" "is big-endian" \
    "$tmp/cross/main.sword" --target=s390x-ibm-linux -c
refused "--target with no runtime for it" "make TARGET=x86_64-sword-none" \
    "$tmp/cross/main.sword" --target=x86_64-sword-none -o "$tmp/cross/prog"
refused "shield test --target" "test runs what it builds" \
    test "$tmp/picked" --target=x86_64-unknown-linux-gnu

# A reduction has to answer the same thing whatever the pool looks like. The
# float sum is the one that shows it: adding the pieces in the order they
# finish rounds differently from adding them in the order they were cut, so
# before the partials this printed a different number for every thread count.
# The second one is a reduction the program defines rather than an operator the
# compiler knows, which has to hold to the same rule.
for name in reducedet userreduce; do
    if ! "$shield" "$root/tests/$name.sword" -o "$tmp/$name" \
            > "$tmp/$name.log" 2>&1; then
        echo "FAIL $name determinism: compilation failed"
        sed 's/^/     /' "$tmp/$name.log"
        fail=$((fail + 1))
        continue
    fi
    answers=$(for threads in 1 2 3 4 8 16; do
        SWORD_THREADS=$threads perl -e 'alarm 60; exec @ARGV' "$tmp/$name"
    done | sort -u | wc -l | tr -d ' ')
    if [ "$answers" != 1 ]; then
        echo "FAIL $name determinism: $answers answers across thread counts"
        for threads in 1 2 3 4 8 16; do
            printf '     %2s: ' "$threads"
            SWORD_THREADS=$threads "$tmp/$name"
        done
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

# A program can be tested too: its own main steps aside for the runner's, and
# the tests still reach everything else it declares (issue #4).
mkdir -p "$tmp/program"
cat > "$tmp/program/main.sword" <<'EOF'
func twice(x int) int {
    return x * 2
}

func main() !int {
    return twice(21)
}
EOF
cat > "$tmp/program/main_test.sword" <<'EOF'
import "std/testing"

func TestTwice(mut t *testing.T) !void {
    try t.Equal(twice(21), 42)
}
EOF
out=$("$shield" test "$tmp/program" 2>&1)
got=$?
if [ "$got" != 0 ] || ! printf '%s\n' "$out" | grep -qF "ok    1 tests"; then
    echo "FAIL shield test on a program: exit $got"
    printf '%s\n' "$out" | sed 's/^/     /'
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi

# What -p is given reaches the test binary as one argument, never as more of
# the command line.
"$shield" test "$tmp/picked" -run TestPasses -p "1; touch $tmp/injected" \
    > /dev/null 2>&1
if [ -e "$tmp/injected" ]; then
    echo "FAIL shield test -p: its value ran as a command"
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi

# `-bench` runs the Benchmark... functions instead of the tests, and finds its
# own N: the body is called with a small one and scaled until a measurement
# lasts long enough to mean anything.
mkdir -p "$tmp/bench"
cat > "$tmp/bench/bench_test.sword" <<'EOF'
import "std/testing"

func TestStillRuns(mut t *testing.T) !void {
    try t.Equal(1 + 1, 2)
}

func BenchmarkAdding(mut b *testing.B) !void {
    mut total i64 = 0
    for i in 0..b.N {
        total += i64(i) % 7
    }
    b.Keep(total)
}
EOF
benched() { # expected exit, expected text, arguments
    want=$1 text=$2
    shift 2
    out=$("$shield" test "$tmp/bench" "$@" 2>&1)
    got=$?
    if [ "$got" != "$want" ] || ! printf '%s\n' "$out" | grep -qF "$text"; then
        echo "FAIL shield test -bench $*: exit $got, want $want with '$text'"
        printf '%s\n' "$out" | sed 's/^/     /'
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}
benched 0 "BenchmarkAdding" -bench
benched 0 "ns/op" -bench
benched 0 "ok    1 tests"                     # without it, the tests run
benched 1 "no benchmark 'BenchmarkNope'" -bench -run BenchmarkNope

# The simulator finds what it is for. Each planted bug is reported with a seed,
# that seed makes it happen again, a deadlock is called one, the correct tests
# pass under every seed, and the network test is skipped rather than run.
sim_out=$(perl -e 'alarm 120; exec @ARGV' "$shield" test "$root/tests/simulated" -sim -seeds 50 2>&1)
sim_check() { # description, pattern
    if printf '%s\n' "$sim_out" | grep -qE "$2"; then
        pass=$((pass + 1))
    else
        echo "FAIL shield test -sim: $1"
        printf '%s\n' "$sim_out" | sed 's/^/     /'
        fail=$((fail + 1))
    fi
}
sim_check "finds the lost update" "^FAIL  TestLostUpdate under seed [0-9]+$"
sim_check "calls the deadlock one" "^FAIL  TestOppositeLocks under seed [0-9]+: deadlock$"
sim_check "skips the network" "^SKIP  TestNetwork: uses the network"
sim_check "reports the run" "^FAIL  4 tests, 2 failed, 1 skipped, 50 seeds each$"
if printf '%s\n' "$sim_out" | grep -qE "^FAIL  (TestAtomicAdd|TestHourLongSleep)"; then
    echo "FAIL shield test -sim: a correct test failed"
    printf '%s\n' "$sim_out" | sed 's/^/     /'
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi
lost_seed=$(printf '%s\n' "$sim_out" | sed -n 's/^FAIL  TestLostUpdate under seed \([0-9]*\)$/\1/p')
again=0
for i in 1 2 3; do
    "$shield" test "$root/tests/simulated" -sim -seed "$lost_seed" \
        -run TestLostUpdate > "$tmp/sim.again" 2>&1 && continue
    grep -q "got 1, want 2" "$tmp/sim.again" && again=$((again + 1))
done
if [ "$again" = 3 ]; then
    pass=$((pass + 1))
else
    echo "FAIL shield test -sim: seed $lost_seed reproduced $again times of 3"
    fail=$((fail + 1))
fi

# Any program runs simulated under SWORD_SIM_SEED: the clock is virtual, the
# same seed gives the same run, and different seeds explore different orders.
"$shield" "$root/tests/simprograms/clock.sword" -o "$tmp/simclock" \
    > "$tmp/simclock.log" 2>&1
SWORD_SIM_SEED=1 perl -e 'alarm 10; exec @ARGV' "$tmp/simclock" > /dev/null 2>&1
got=$?
if [ "$got" = 42 ]; then
    pass=$((pass + 1))
else
    echo "FAIL simulated clock: exit $got, want 42"
    fail=$((fail + 1))
fi
if "$shield" "$root/tests/simprograms/order.sword" -o "$tmp/simorder" \
        > "$tmp/simorder.log" 2>&1; then
    same=0
    orders=$(for seed in $(seq 1 20); do
        a=$(SWORD_SIM_SEED=$seed "$tmp/simorder")
        b=$(SWORD_SIM_SEED=$seed "$tmp/simorder")
        [ "$a" = "$b" ] && echo "$a"
    done)
    kept=$(printf '%s\n' "$orders" | grep -c .)
    distinct=$(printf '%s\n' "$orders" | sort -u | grep -c .)
    if [ "$kept" = 20 ] && [ "$distinct" -ge 3 ]; then
        pass=$((pass + 1))
    else
        echo "FAIL simulated order: $kept of 20 seeds repeated, $distinct orders"
        fail=$((fail + 1))
    fi
else
    echo "FAIL simulated order: compilation failed"
    fail=$((fail + 1))
fi

# `--sim` puts a switch point before every atomic operation; without it the
# code is what it always was.
with_points=$("$shield" "$root/tests/atomics.sword" --sim --emit-llvm 2>&1 | grep -c "call void @sword_sim_point")
without=$("$shield" "$root/tests/atomics.sword" --emit-llvm 2>&1 | grep -c "sword_sim_point")
if [ "$with_points" -gt 0 ] && [ "$without" = 0 ]; then
    pass=$((pass + 1))
else
    echo "FAIL --sim: $with_points switch points with it, $without without"
    fail=$((fail + 1))
fi

# A call out of the language is said out loud under the simulator, which
# cannot replay it, and nowhere else.
warned=$("$shield" "$root/tests/defer.sword" --sim -o "$tmp/simwarn" 2>&1 | grep -c "warning: this calls out of the language (write)")
quiet=$("$shield" "$root/tests/defer.sword" -o "$tmp/simwarn" 2>&1 | grep -c "warning")
if [ "$warned" -gt 0 ] && [ "$quiet" = 0 ]; then
    pass=$((pass + 1))
else
    echo "FAIL --sim: $warned warnings about calling out with it, $quiet without"
    fail=$((fail + 1))
fi

# Both programs say the version the source says, and the same one.
want_version=$(sed -n 's/^#define SWORD_VERSION "\(.*\)"/\1/p' "$root/src/version.h")
for program in shield swordls; do
    said=$("$root/$program" --version 2>&1)
    case $said in
    "$program $want_version ("*")") pass=$((pass + 1)) ;;
    *)
        echo "FAIL $program --version: '$said', want $want_version"
        fail=$((fail + 1))
        ;;
    esac
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
