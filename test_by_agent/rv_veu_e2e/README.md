# RV-VEU VADD End-to-End Test

This test checks the full path from the RV pipeline to the timing VEU model:

```text
RV fetch -> RV decode VEU instruction -> CBU CSR handshake -> RV continues
-> TimingVeu DMEM read/VADD/DMEM write in background
-> RV polls VEUSTATUS busy -> RV loads result
```

Run it from the repository root:

```sh
bash test_by_agent/rv_veu_e2e/run.sh
```

The script generates:

- `instr_mem.hex`: RV program with VEU CSR setup, `vadd`, status polling, result loads, and `ebreak`.
- `data_mem.hex`: byte-format DMEM image with input vectors at `0x100` and `0x200`.

The gem5 output directory is:

```text
test_by_agent/rv_veu_e2e/m5out_veu_vadd/
```

Important outputs:

- `run.log`: gem5 stdout/stderr and RV retire trace.
- `stats.txt`: gem5 statistics.
- `result_summary.txt`: parsed PASS/FAIL result, lane values, and VEU stats.

Expected result:

```text
RV-VEU VADD E2E PASS

Results:
  lane0 actual=0x0000000b expected=0x0000000b
  lane7 actual=0x00000058 expected=0x00000058

Stats:
  veu_issue_count=<configuration, vadd, and polling VEU instructions>
  veu_complete_count=<same as veu_issue_count>
  veu_csr_handshake_cycles=<RV cycles stalled for CSR handshakes>
  rv_dmem_blocked_by_veu_cycles=<RV load/store cycles blocked by TimingVeu DMem ownership>
  veu_operation_start_count=1
  veu_operation_complete_count=1
  veu_busy_cycles=<VEU background operation cycles>
  veu_chunks=1
  veu_memory_reads=2
  veu_memory_writes=1
  cycle_count=<full polling test cycles>
  stall_count=<all RV pipeline stalls>
  flush_count=<taken polling branches>
```
