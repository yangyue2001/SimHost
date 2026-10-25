#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SIM=./build/sim-dram
OFF=./build/sim

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

mkdir -p build

if ! "$SIM" --reset-cycles=2 --max-cycles=10000 >build/dram.out 2>build/dram.err; then
  cat build/dram.out build/dram.err >&2
  fail "dram proof command"
fi
cat build/dram.out
grep -q "DRAMsim3 memory system initialized." build/dram.out || fail "library banner"
grep -q "GOOD TRAP" build/dram.out || fail "good trap"
delay=$(sed -n 's/^DRAMSim3 read delay //p' build/dram.out | head -n 1)
latency=$(sed -n 's/^DRAM proof latency \([0-9][0-9]*\) data ok$/\1/p' build/dram.out | head -n 1)
if [ -z "$delay" ] || [ -z "$latency" ]; then
  fail "missing delay lines"
fi
if [ "$delay" -lt 100 ] || [ "$latency" -lt 100 ]; then
  fail "delay $delay latency $latency is below 100"
fi

if "$SIM" --reset-cycles=2 --max-cycles=20 --dramsim3-ini=/no/such-dramsim3.ini \
    >build/dram-bad-ini.out 2>build/dram-bad-ini.err; then
  fail "missing ini was accepted"
fi
grep -q "cannot open DRAMSim3 config file" build/dram-bad-ini.err || fail "missing ini text"

if [ ! -x "$OFF" ]; then
  echo "phase 1 binary is missing; build it with: make smoke" >&2
  fail "phase 1 binary"
fi
if "$OFF" --dramsim3-ini=/no/such-dramsim3.ini >build/dram-disabled.out 2>build/dram-disabled.err; then
  fail "disabled binary accepted dramsim3 ini"
fi
grep -q "DRAMSim3 is not enabled" build/dram-disabled.err || fail "disabled ini text"

echo "dramsim3 checks passed"
