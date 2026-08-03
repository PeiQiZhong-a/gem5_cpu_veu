# BRS CPU cycle alignment with dut_kui

Two independent RTL-testbench platforms are available:

- `rtl-dut-kui-tb` preserves the legacy 256-bit, multi-bank model;
- `rtl-npu-lpnpu-mikui` follows `npu_lpnpu` `mikui_v2.0` with two native
  128-bit banks.

The legacy `rtl-dut-kui-tb` model keeps the earlier Spirit CPU-facing memory
system:

- independent 128-bit instruction SRAM path;
- 32-bit DBus through the registered 32-to-256 converter;
- native 256-bit VEU path and a 128-bit SAU endpoint adapted onto the legacy
  256-bit crossbar path;
- four decoded data ports, of which banks 0-2 are real SRAM and port 3 is the
  RTL dummy port;
- tick-level crossbar state, bank arbitration, response latency, start/done
  sidebands. The frozen `dut_mikui` CPU interrupt pins do not receive SAU
  `crossbarDone`.

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
- configured external interrupt bit 3 (normally zero for `dut_mikui`);
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

## Run the npu_lpnpu Mikui model

```powershell
build\RISCV\gem5.opt --outdir=m5out `
  configs\brs\run_pipeline_mini.py `
  --mem-system rtl-npu-lpnpu-mikui `
  --program-file instruction.hex `
  --dmem-hex memory.hex `
  --cycle-trace gem5_mikui_cycle_trace.log
```

This platform fixes instruction SRAM to `0x00000000-0x00003fff`, data bank 0
to `0x20010000-0x2001ffff`, and data bank 1 to
`0x20020000-0x2002ffff`. As in `top_mikui_tb.sv`, one data image initializes
both physical banks. External, software, and timer CPU interrupts are tied to
zero.

The Crossbar reproduces RTL edge behavior rather than a configurable aggregate
latency: start takes priority over DBUS in IDLE, ACTIVE executes on the edge
that samples done, RVACTIVE is sticky until accelerator start, the bank request
is registered, slave acknowledgement follows that request by one register,
and the master bank selector has two registers. The declared RTL
`SRAM_RESP_DELAY` parameter does not affect the implementation.

## Compare traces

```powershell
python util\brs\compare_cycle_traces.py `
  rtl_cycle_trace.log m5out\gem5_cycle_trace.log `
  --window 12 --json-report cycle_compare.json
```

The comparator stops at the first mismatch. Payload fields are compared only
when their corresponding valid signal is asserted.
