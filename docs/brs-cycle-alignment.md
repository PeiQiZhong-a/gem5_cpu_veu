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

Cycle traces use the strict [`brs-cycle-trace-v3`](brs-cycle-trace-v3.md)
contract. Reset edges are excluded, and `phase=posedge-pre-nba` records the
logical state visible immediately before the active edge commits its state
updates. State changes made by that edge appear in the following record.

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
  --program-file instruction_words.hex `
  --dmem-hex data_mem.hex `
  --cycle-trace gem5_cycle_trace.log
```

`--mem-system` defaults to `rtl-dut-kui-tb`. The instruction image may instead
be provided with `--imem-image`; use only one instruction image input. This
mode maps the RTL application instruction SRAM at `0x29110000..0x2911ffff`
and defaults the CPU reset PC to `0x29110008`. Use `--entry-point` only when a
synthetic image intentionally starts at another address inside that window.

## Compare traces

```powershell
python util\brs\compare_cycle_traces.py `
  rtl_cycle_trace.log m5out\gem5_cycle_trace.log `
  --anchor-retire-pc 0x29110008 --stop-at-done `
  --window 12 --json-report cycle_compare.json
```

The application anchor is required when RTL starts in the boot ROM but gem5
uses the direct-application memory model.  Each trace is rebased at the first
retirement of the entry PC and truncated at its first DONE store; everything
inside that application window remains a strict edge-by-edge comparison.  The
comparator stops at the first mismatch. Payload fields are compared only when
their corresponding valid signal is asserted.
