#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SIM=./build/sim
OFF=./build/sim-off

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

expect_ok() {
  name=$1
  shift
  if ! "$@" >"$name.out" 2>"$name.err"; then
    echo "command failed: $*" >&2
    cat "$name.out" "$name.err" >&2
    fail "$name"
  fi
}

expect_fail() {
  name=$1
  shift
  if "$@" >"$name.out" 2>"$name.err"; then
    echo "command unexpectedly succeeded: $*" >&2
    cat "$name.out" "$name.err" >&2
    fail "$name"
  fi
}

mkdir -p build

expect_ok build/help "$SIM" --help
grep -q -- "--no-assert" build/help.out || fail "help assert option"
expect_fail build/bad-option "$SIM" --not-an-option
expect_fail build/bad-reset "$SIM" --reset-cycles=0

expect_ok build/limit "$SIM" --reset-cycles=2 --max-cycles=10
grep -q "EXCEEDING CYCLE LIMIT at cycle 10" build/limit.out || fail "limit text"
if grep -q "GOOD TRAP" build/limit.out; then
  fail "limit run reached the DUT trap"
fi

expect_ok build/good "$SIM" --reset-cycles=2 --max-cycles=10000
grep -q "GOOD TRAP at cycle 200" build/good.out || fail "good trap text"

expect_ok build/noassert "$SIM" --reset-cycles=2 --max-cycles=10000 --no-assert
grep -q "GOOD TRAP at cycle 200" build/noassert.out || fail "no-assert trap text"

expect_ok build/assert "$SIM" --reset-cycles=2 --max-cycles=10000 --no-assert --assert
grep -q "GOOD TRAP at cycle 200" build/assert.out || fail "assert trap text"

expect_ok build/pos "$SIM" --reset-cycles=2 --max-cycles=8 --dump-wave --wave-path=build/pos.vcd
python3 scripts/check_wave.py build/pos.vcd pos

expect_ok build/full "$SIM" --reset-cycles=2 --max-cycles=8 --dump-wave-full --wave-path=build/full.vcd
python3 scripts/check_wave.py build/full.vcd full

expect_ok build/empty "$SIM" --reset-cycles=1 --max-cycles=3 --dump-wave --log-begin=100 --wave-path=build/empty.vcd
python3 scripts/check_wave.py build/empty.vcd empty

expect_fail build/notrace "$OFF" --dump-wave
grep -q "built without EMU_TRACE" build/notrace.err || fail "notrace message"

echo "phase 1 checks passed"
