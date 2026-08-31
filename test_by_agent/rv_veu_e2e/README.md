# RV-VEU Timing-Profile End-to-End Matrix

This test exercises the complete RV pipeline to TimingVeu path:

```text
RV fetch/decode -> VEU CSR handshake -> TimingVeu memory traffic
-> VFU/VSU -> VEU writeback -> RV polls VEUSTATUS -> RV validates every result word
```

Run the matrix from the repository root:

```sh
bash test_by_agent/rv_veu_e2e/run.sh
```

## Coverage

The runner executes 22 functional variants at both `VLEN=256` and
`VLEN=2048` bits:

```text
vadd_vector, vadd_scalar, vsub, vmin, vmax,
vredmin, vredmax, vand, vor, vxor, vmv,
vssrl_scalar, vssra_scalar, vnclip_scalar, vredsum, vmul,
vslideup_scalar, vslidedown_scalar, vwredsum, vmsub, vmac, vmulh
```

This is 44 independent gem5 runs. `256` bits covers one 32-byte chunk;
`2048` bits covers eight chunks. All tests use a full write mask. Partial and
zero-mask behavior remains covered by the TimingVeu unit tests.

The v4 profile now contains 64 exact RTL-derived timing rows. Every tuple used
by this 44-case full-mask matrix reports `timing_source=rtl_sim` and
`control_timing_source=rtl_sim`; the required source split is therefore
44 `rtl_sim` and 0 `default` for both data-path and control timing. Other
unmeasured mask/source/scalar tuples still use `VeuTimingConfig` defaults and
remain explicitly labeled `default`.

`vwredsum` and `vmulh` exercise the datapaths visible in the current RTL while
also requiring one illegal-operation indication, matching the VCU status
decode. `vcompress` and `vmulhsu` are omitted because the current RTL does not
provide a complete selectable datapath for them.

## What each case checks

The generator writes deterministic source vectors and an independent expected
result. After VEU completes, the RV program reads every 32-bit word of the
destination vector, ORs any mismatch into `x10`, and ends with `ebreak`.
The verifier requires final `x10=0` and also checks:

- exactly one VEU operation starts and finishes;
- chunk, read, and write counts match the operation source/write policy;
- profile hit/fallback and `rtl_sim`/`default` source counters match each
  operation/scalar/source/mask/chunk timing combination; mode remains in
  functional data handling and trace output but does not affect timing source;
- `operation_start` carries data-path and control timing sources and evidence
  IDs, plus `lock_start_delay` and `finish_drain_cycles`;
- illegal-operation count matches the current RTL VCU decode;
- no unexpected response or remaining outstanding read;
- outstanding reads never exceed four;
- trace request/response transaction IDs match;
- trace read/write addresses match all expected 32-byte chunks;
- simulation exits before its 10,000-cycle limit.

## Outputs

All generated inputs and gem5 outputs are under:

```text
test_by_agent/rv_veu_e2e/m5out_veu_matrix/
```

Important files are:

- `summary.csv`: one PASS/FAIL row for every operation and VLEN.
- `<case>_<vlen>/instr_mem.hex`, `data_mem.hex`, `metadata.json`: generated
  self-checking program, input memory, and expected structural metadata.
- `<case>_<vlen>/run.log`: gem5 log and RV retire trace.
- `<case>_<vlen>/stats.txt`: aggregate gem5 and VEU statistics.
- `<case>_<vlen>/veu_cycle_trace.csv`: VEU events, transaction IDs, FIFO,
  outstanding read, status, and lock states.
- `<case>_<vlen>/verify.log`: failure reason when a case does not pass.

The runner always passes the normalized timing profile and emits a separate
cycle trace for every case. This matrix validates model functionality and
internal timing/transaction consistency; it does not compare gem5 cycles or
traces against RTL.

## Shared RTL/gem5 firmware conversion

Convert the archived 44-case local-address matrix into the current RTL
application address map with:

```sh
python3 test_by_agent/rv_veu_e2e/convert_veu_matrix_shared.py \
  --source /home/zpq/文档/m5out_veu_matrix \
  --output /home/zpq/文档/m5out_veu_matrix_shared
```

Each converted case contains `instr_mem.hex` and `data_mem.hex` for gem5,
plus packed 128-bit `instruction.hex` and `memory.hex` for the RTL QSPI
testbench.  The instruction image contains the two-word boot header, executes
from `0x29110008`, uses data addresses under `0x29120000`, and reports the
self-check result by writing PASS (`0x2`) or FAIL (`0x4`) to `0x4001e004`.
The root `manifest.json` records SHA-256 hashes for every generated file.

Run the converted matrix sequentially and generate a CPU/VEU trace pair for
every case with:

```sh
test_by_agent/rv_veu_e2e/run_shared_matrix.sh \
  --gem5 /path/to/build/RISCV/gem5.opt \
  --input-root /home/zpq/文档/m5out_veu_matrix_shared \
  --output-root /home/zpq/文档/m5out_veu_matrix_shared_runs
```

The runner continues after individual failures and writes `summary.csv`.
Every case output contains `brs_cycle_trace.log`, `veu_cycle_trace.csv`,
`run.log`, `stats.txt`, the exact `command.sh`, and verification output. It
also requires a `done=1` controller-status write and rejects `error=1` in the
CPU trace. Use `--case vmac_256` for a single-case smoke test or `--dry-run`
to inspect the complete command list.

## Current dut_kui memory path: focused VADD

The focused runner routes TimingVEU through the internal cycle model of the
current `dut_kui` 256-bit SRAM path instead of gem5 `SimpleMemory`:

```sh
bash test_by_agent/rv_veu_e2e/run_dut_kui_vadd.sh
```

It runs non-scalar VADD at VLEN 256 and 2048 with config `0x700`, the current
RTL data base `0x29120000`, and the checked RTL input pattern `0xfd + 0x01`.
The RV program checks every destination word. The timing verifier additionally
requires four-cycle VEU read returns and one-cycle write completion, then writes
a gem5/RTL comparison to `m5out_dut_kui_vadd/timing_compare.csv` using the
checked-in `yinglong_veu_timing_fixed2` report as the RTL reference.

## dut_kui matrix from captured RTL examples

The full DUT-memory-path regression consumes every checked RTL functional
vector directly instead of regenerating unrelated operands:

```sh
bash test_by_agent/rv_veu_e2e/run_dut_kui_rtl_matrix.sh
```

It currently runs all 33 production captures and all 20 supplemental captures.
This covers the measured operations, VLEN 256/512/1024/2048, all four VADD
modes, vector/scalar VADD examples, full/partial/zero write masks, and five
captured scalar values. Each generated SRAM image uses the captured RTL
sources and result as its golden data.

The v4 timing profile records `operation_cycles`, `lock_start_delay`, and
`finish_drain_cycles` from matching RTL evidence. Mode-independent timing
tuples use the full RTL row; unmatched scalar/source/mask/chunk tuples use
`VeuTimingConfig` defaults and are labeled `default`. The reader remains
compatible with v1/v2/v3 profiles, whose mode column is also ignored for
timing selection and whose absent control fields are supplied by defaults.

The matrix strictly compares start/status/lock events, read issue and FIFO
pushes, VFU accept/completion, write and bank-write events, chunks, sources,
relative address progression, outstanding reads, FIFO peaks, and store
priority cycles. Absolute gem5 addresses are checked against the remapped E2E
memory image.

TimingVEU models the measured fixed control boundaries structurally:
`operation_start/status_set` at offset 0, `lock_start` at offset 1, four
startup cycles before the first load, a registered SRAM-return/FIFO boundary,
and `lock_finish` four cycles after the measured status-clear edge. The status
edge is independent of final SRAM write completion for operations where RTL
exposes that ordering; internal work and the lock remain active until the
measured finish edge and complete drain. `VLEN=0` remains a CSR-only no-op,
reset mask is zero, and programs that write results explicitly configure
`0xffffffff`. The current regression result is functional `PASS=53/53`,
total-cycle `MATCH=53/53`, and event `EVENT_MATCH=53/53`.

## Complete RTL-gap handoff matrix

The 34-case handoff under `../../rtl_test_case` is checked without modifying
the captured artifacts:

```sh
BRS_VEU_RTL_GAP_OUTROOT=/tmp/veu_rtl_gap \
  bash test_by_agent/rv_veu_e2e/run_dut_kui_rtl_gap_matrix.sh
```

The runner validates the handoff schema, all eight artifact SHA256 values and
the cross-file case set before comparing gem5. It covers scalar VADD/VNCLIP
c8 plus slide, min/max reduction, MAC/MSUB, widening reduction, and multiply
high at chunk classes 1/2/4/8. Ordered timing patterns are compared without
averaging. The retained pre-accept `vredmin c1` RTL completion pulse is treated
as an unowned monitor observation, not as a gem5 result token.

The accepted result is functional `34/34`, total-cycle `34/34`, and exact
comparable-event `34/34`. This includes the nonuniform `vslidedown`
`2;...;2;3` completion pattern, min/max c2 same-address double writes,
source2-first MAC/MSUB read rotation, and `illegal_completed` behavior for
`vwredsum` and `vmulh`. Capture fields unavailable from the recovered monitor
remain reported as `N/A`; they are not converted into passing comparisons.
