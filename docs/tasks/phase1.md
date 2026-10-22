# phase1: platform and empty service table

Status: verified

## Objective

- Trigger: `make check`.
- Observable result: `build/sim` resets, honors global arguments, traces only the rising half-cycle unless `--dump-wave-full` is set, and exits through the `exit` pin. The service table is empty. DRAMSim3 is not required.

## Non-goals

- Memory service, DRAMSim3, probe DPI, difftest, and LightSSS.
- Adding DPI names to the session loop.

## Scope

- Allowed files/modules: `csrc/platform`, `csrc/session`, `csrc/service/service.h`, `tb/SmokeTop.sv`, `Makefile`, `scripts/check_phase1.sh`, `scripts/check_wave.py`, this task note.
- Interfaces that may change: new phase-1 platform and `SimService` contract.
- Inputs and reference implementations: `docs/verilator-sim-plan.md`; read-only behavior reference `/home/yang/study_dir/rvbosc/difftest`.

## Constraints

- Compatibility/API/ABI: session code must not name a concrete service. `SimTop` ports are `clock`, `reset`, and `exit`.
- Runtime, resource, security, or performance limits: phase 1 is a tiny cycle-driven model.
- Required tools and versions: Verilator 5.038 and g++.
- Permission or external-side-effect boundary: write only this repo and waveform files under `build/` or the path given by `--wave-path`.

## Acceptance

- [x] `env -u DRAMSIM3_HOME make check`
- [x] `--dump-wave` samples keep `clock` at 1
- [x] `--dump-wave-full` samples alternate `clock` 1 and 0
- [x] a no-trace binary returns non-zero for `--dump-wave`
- [x] `--max-cycles=10` returns 0 before the DUT good trap

## Assumptions and open decisions

- Assumption: `--seed=0` selects Verilator `randReset(0)`; a non-zero seed selects `randSeed` plus `randReset(2)`.
- Assumption: the default traced binary is `build/sim`. `EMU_TRACE=0` writes `build/sim-off`, and `EMU_TRACE=fst` writes `build/sim-fst`.
- Open question (only if blocking): none.

## Slices

1. [x] Build the empty-service session, platform, and smoke DUT — verify with `make check`; stop if waveform edge selection or the exit code is wrong.

## Evidence

- Commands actually run: `env -u DRAMSIM3_HOME make check` passed. Posedge trace had 8 samples, all `clock=1`. Full trace had 20 alternating samples. The log window outside the run recorded no samples. Limit stopped at cycle 10. The DUT reached `GOOD TRAP at cycle 200`.
- First failure or baseline failure: the VCD checker initially missed indented `$var` lines; it now strips them. The build uses `g++` from `PATH`.
- Artifacts/logs/checkpoint: `build/sim`, `build/sim-off`.
- Remaining risk: FST was not executed in `make check`. The same waveform class selects `VerilatedFstC` when `EMU_TRACE=fst`.
