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

The runner executes the 16 timing-profile variants in
`configs/brs/veu_timing_profile.csv` at both `VLEN=256` and `VLEN=2048` bits:

```text
vadd_vector, vadd_scalar, vsub, vmin, vmax,
vredmin, vredmax, vand, vor, vxor, vmv,
vssrl_scalar, vssra_scalar, vnclip_scalar, vredsum, vmul
```

This is 32 independent gem5 runs. `256` bits covers one 32-byte chunk;
`2048` bits covers eight chunks. All tests use a full write mask. Partial and
zero-mask behavior remains covered by the TimingVeu unit tests.

`vredmin` and `vredmax` match their checked-in compatibility profile rows;
they are profile hits but are not claims of RTL cycle calibration.

## What each case checks

The generator writes deterministic source vectors and an independent expected
result. After VEU completes, the RV program reads every 32-bit word of the
destination vector, ORs any mismatch into `x10`, and ends with `ebreak`.
The verifier requires final `x10=0` and also checks:

- exactly one VEU operation starts and finishes;
- chunk, read, and write counts match the operation source/write policy;
- profile hit is one, profile miss/fallback is zero;
- no illegal operation, unexpected response, or remaining outstanding read;
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
