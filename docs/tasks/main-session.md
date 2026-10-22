# main-session: process entry owns the outcome

Status: verified

## Objective

- Trigger: `make check`.
- Observable result: the phase 1 binary behavior is unchanged. `main` initializes the process, opens one session, runs it, and prints the trap. The cycle loop stays in the session.

## Non-goals

- Memory service, DRAMSim3, probe DPI, difftest, and LightSSS.
- Moving clock edges, service calls, or DPI names into `main`.
- Changing CLI flags, waveform rules, or exit codes.

## Scope

- Allowed files/modules: `csrc/session/main.cpp`, `csrc/session/session.h`, `csrc/session/session.cpp`, `Makefile`, `docs/verilator-sim-plan.md`, this note.
- Interfaces that may change: `run_session` is replaced by `Session::open` and `Session::run`. `SimService` and `Platform` stay the same.
- Inputs and reference implementations: `docs/verilator-sim-plan.md`; difftest `src/test/csrc/emu/main.cpp` is the shape reference only.

## Constraints

- Compatibility/API/ABI: `scripts/check_phase1.sh` output and exit codes stay the same. Session code still must not name a concrete service.
- Runtime, resource, security, or performance limits: no new per-cycle work.
- Required tools and versions: Verilator 5.038, `g++` from `PATH`, `bear` for `compile_commands.json`.
- Permission or external-side-effect boundary: write only this repo. The compilation database may contain absolute host paths.

## Acceptance

- [x] `env -u DRAMSIM3_HOME make check`
- [x] `main` does not call `set_clock`, `eval`, or a concrete service
- [x] `compile_commands.json` exists at the repo root and lists `csrc/session/main.cpp`

## Assumptions and open decisions

- Assumption: startup failures still print their existing stderr text inside the session and return to `main` as "not started". `main` turns that into exit code 1 and does not print a trap line.
- Open question (only if blocking): none.

## Slices

1. [x] Split process entry from the run script — verify with `make check`; stop if a trap line, exit code, or waveform check changes.

## Evidence

- Commands actually run: `env -u DRAMSIM3_HOME make check` passed. Posedge trace had 8 samples, all clock=1. Full trace had 20 alternating samples. The empty window recorded no samples. `./build/sim --max-cycles=10` printed `EXCEEDING CYCLE LIMIT at cycle 10` and returned 0. `./build/sim-off --dump-wave` returned 1 and printed the existing no-trace message without a trap line.
- First failure or baseline failure: none.
- Artifacts/logs/checkpoint: `compile_commands.json` has 12 entries. The eight csrc files use the csrc include path, `-DEMU_TRACE_VCD`, and the Verilator include path. `directory` is `build/obj-vcd`, so `VTop.h` resolves.
- Remaining risk: the database records the default VCD build only. `EMU_TRACE=0` and FST are not separate compilation databases. Regenerate with `make compile-commands` after changing sources or trace mode.
