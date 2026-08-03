# BRS CPU cycle alignment with dut_kui

`rtl-dut-kui-tb` is the only internal RTL-testbench memory model. It models
the current Spirit CPU-facing memory system:

- independent 128-bit instruction SRAM path;
- 32-bit DBus through the registered 32-to-256 converter;
- shared 256-bit SAU and VEU master paths;
- four decoded data ports, of which banks 0-2 are real SRAM and port 3 is the
  RTL dummy port;
- tick-level crossbar state, bank arbitration, response latency, start/done
  sidebands, and SAU external interrupt bit 3.

## Sampling contract

Cycle traces use `brs-cycle-trace-v2`.
`phase=evaluate` records signals visible immediately before the closing clock
edge. State changes made by that edge appear in the following tick.

Important fields include:

- `converter_state_pre` and `xbar_state_pre`;
- CPU IBus/DBus requests and responses;
- HC request, target, completion, and result;
- SAU SRAM request/response and crossbar start/done;
- SAU/VEU accepted and dropped beats;
- same-bank collision and bank request mask;
- dynamic SAU interrupt bit 3;
- retirement, writeback, and pipeline stall state.

## Run the current model

```powershell
build\RISCV\gem5.opt --outdir=m5out `
  configs\brs\run_pipeline_mini.py `
  --mem-system rtl-dut-kui-tb `
  --program-file inst_mem.hex `
  --dmem-hex data_mem.hex `
  --cycle-trace gem5_cycle_trace.log
```

`--mem-system` defaults to `rtl-dut-kui-tb`. The instruction image may instead
be provided with `--imem-image`; use only one instruction image input.

## Compare traces

```powershell
python util\brs\compare_cycle_traces.py `
  rtl_cycle_trace.log m5out\gem5_cycle_trace.log `
  --window 12 --json-report cycle_compare.json
```

The comparator stops at the first mismatch. Payload fields are compared only
when their corresponding valid signal is asserted.
