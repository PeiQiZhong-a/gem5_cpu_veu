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

The runner executes the 18 normal Mikui operation variants at both
`VLEN=128` and `VLEN=1024` bits:

```text
vadd_vector, vadd_scalar, vsub, vmin, vmax,
vredmin, vredmax, vand, vor, vxor,
vslideup_scalar, vslidedown_scalar, vmv,
vssrl_scalar, vssra_scalar, vnclip_scalar, vredsum, vmul
```

This is 36 independent gem5 runs. `128` bits covers one 16-byte chunk;
`1024` bits covers eight chunks. All tests use the Mikui full `0xffff` write
mask. Partial and
zero-mask behavior remains covered by the TimingVeu unit tests.

The checked-in profile rows are explicitly `legacy_default`: profile hits are
functional/structural coverage and are not claims of Mikui RTL cycle calibration.

## What each case checks

The generator writes deterministic source vectors and an independent expected
result. After VEU completes, the RV program reads every 32-bit word of the
destination vector, ORs any mismatch into `x10`, and ends with `ebreak`.
The verifier requires final `x10=0` and also checks:

- exactly one VEU operation starts and finishes;
- chunk, read, and write counts match the operation source/write policy;
- profile hit is one, profile miss/fallback is zero, and the timing source is
  reported as legacy rather than RTL simulation;
- no illegal operation, unexpected response, or remaining outstanding read;
- outstanding reads never exceed four;
- trace request/response transaction IDs match;
- trace read/write addresses match all expected 16-byte chunks;
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

## `dut_mikui_dma` RTL-calibrated full matrix

The workstation-derived profile and terminal tables are covered by the
three-SRAM `dut_mikui_dma` topology with:

```sh
python3 test_by_agent/rv_veu_e2e/run_mikui_dma_timing_matrix.py
```

This runs all 207 normal profile rows and all 27 terminal rows that can be
encoded by the RV custom instruction path. It also runs the direct TimingVeu
test for the one `unknown_start_bit` row that deliberately has no CPU opcode.
The checked dimensions are operation, scalar/vector source selection,
chunks 1/2/4/8, and full/partial/zero mask.

Every E2E PASS requires all of the following:

- the RV destination self-check ends with `x10=0`;
- `operation_finish - operation_start` equals the workstation cycle count;
- the status-clear offset equals the workstation count;
- the exact profile or terminal ID is selected with no fallback;
- read/write addresses and transaction counts match the measured policy;
- every accepted transaction receives one response and no read remains
  outstanding at simulation exit.

Results are written to:

```text
m5out/veu_timing_matrix/
```

`summary.csv` is the machine-readable result for every row and `summary.md`
is the concise result. Each normal or terminal subdirectory retains its own
program, data image, metadata, `stats.txt`, `veu_trace.csv`, `run.log`, and
`verify.json`.

## Native Mikui memory path: focused VADD

The focused runner routes TimingVEU through the two-bank 128-bit model of
`dut_mikui.sv` instead of gem5 `SimpleMemory`:

```sh
bash test_by_agent/rv_veu_e2e/run_dut_kui_vadd.sh
```

It runs non-scalar VADD at VLEN 128 and 1024, uses Mikui bank 0 at
`0x20010000`, and verifies every destination word. It intentionally does not
compare cycles with Yinglong: a Mikui RTL timing capture must be supplied before
the simulator can claim cycle alignment.
