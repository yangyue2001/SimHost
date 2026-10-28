#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SIM=./build/sim-dram

fail() {
  echo "FAIL: $1" >&2
  if [ -f build/lightsss.out ]; then
    cat build/lightsss.out >&2
  fi
  if [ -f build/lightsss.err ]; then
    cat build/lightsss.err >&2
  fi
  exit 1
}

mkdir -p build

set +e
timeout 30 "$SIM" --reset-cycles=2 --max-cycles=10000 --fork \
  --fork-interval=10 --fork-fail-at=250 \
  >build/lightsss.out 2>build/lightsss.err
code=$?
set -e

cat build/lightsss.out
if [ -s build/lightsss.err ]; then
  cat build/lightsss.err >&2
fi
if [ "$code" -ne 1 ]; then
  fail "exit code $code, expected 1"
fi

grep -q "DRAMsim3 memory system initialized." build/lightsss.out || fail "dramsim3 banner"
grep -q "DRAM proof latency" build/lightsss.out || fail "dram proof"
grep -q "LightSSS checkpoint pid" build/lightsss.out || fail "no checkpoint"
grep -q "LightSSS inject abort at cycle 250" build/lightsss.out || fail "inject"
grep -q "LightSSS wakeup oldest" build/lightsss.out || fail "wakeup"
grep -q "LightSSS child" build/lightsss.out || fail "child resume"
grep -q "LightSSS reached abort cycle 250" build/lightsss.out || fail "replay"
grep -q "LightSSS step forward done at cycle 350" build/lightsss.out || fail "step"
grep -q "ABORT" build/lightsss.out || fail "abort line"

delays=$(grep -c '^DRAMSim3 read delay ' build/lightsss.out || true)
if [ "$delays" -lt 2 ]; then
  fail "expected parent and child DRAMSim3 delays, got $delays"
fi

echo "lightsss checks passed"
