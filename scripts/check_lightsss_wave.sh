#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SIM=./build/sim-lightsss-wave

fail() {
  echo "FAIL: $1" >&2
  if [ -f build/lightsss-wave.out ]; then
    cat build/lightsss-wave.out >&2
  fi
  if [ -f build/lightsss-wave.err ]; then
    cat build/lightsss-wave.err >&2
  fi
  exit 1
}

mkdir -p build
rm -f sim.vcd sim_*.vcd

set +e
timeout 60 "$SIM" --reset-cycles=2 --max-cycles=10000 --fork \
  --fork-interval=10 --fork-fail-at=250 --dump-wave --wave-path=sim.vcd \
  >build/lightsss-wave.out 2>build/lightsss-wave.err
code=$?
set -e

cat build/lightsss-wave.out
if [ -s build/lightsss-wave.err ]; then
  cat build/lightsss-wave.err >&2
fi
if [ "$code" -ne 1 ]; then
  fail "exit code $code, expected 1"
fi

grep -q "LightSSS parent will not dump a waveform" build/lightsss-wave.out || fail "parent defer"
if [ -f sim.vcd ]; then
  fail "parent opened sim.vcd"
fi
wave=$(grep -o 'dump wave to sim_[0-9][0-9]*\.vcd' build/lightsss-wave.out | head -n 1 | sed 's/dump wave to //; s/\.\.\.//')
if [ -z "$wave" ] || [ ! -s "$wave" ]; then
  fail "child waveform $wave"
fi
grep -q "LightSSS step forward done at cycle 350" build/lightsss-wave.out || fail "step"
grep -q "ABORT" build/lightsss-wave.out || fail "abort line"

echo "lightsss waveform checks passed"
