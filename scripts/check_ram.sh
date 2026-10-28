#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SIM=./build/sim-ram

fail() {
  echo "FAIL: $1" >&2
  if [ -f build/ram.out ]; then
    cat build/ram.out >&2
  fi
  if [ -f build/ram.err ]; then
    cat build/ram.err >&2
  fi
  exit 1
}

mkdir -p build

if ! "$SIM" --reset-cycles=2 --max-cycles=100 >build/ram.out 2>build/ram.err; then
  fail "ram proof command"
fi
cat build/ram.out
grep -q "functional RAM:" build/ram.out || fail "functional ram banner"
grep -q "RAM proof latency 1 data ok" build/ram.out || fail "latency"
grep -q "GOOD TRAP" build/ram.out || fail "good trap"
if grep -q "DRAMsim3" build/ram.out build/ram.err; then
  fail "plain ram used dramsim3"
fi

echo "ram checks passed"
