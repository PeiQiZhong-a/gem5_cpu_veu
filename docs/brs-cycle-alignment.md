# BRS CPU cycle alignment with the Aerith RTL testbench

This flow certifies cycle alignment in three layers:

1. the DONE request must occur on the same rising edge with the same value;
2. every retired instruction must have the same PC, instruction, writeback,
   and retirement edge;
3. CPU IBus/DBus, crossbar grant, stall mask, and redirect must match on every
   meaningful edge.

The comparator stops at the first divergence and prints a small window around
it. Payload pins are compared only when valid, so stale read data does not
create false failures.

## Sampling contract

Both traces use `brs-cycle-trace-v1` and sample the values presented at the
rising edge before nonblocking assignments update registered state.

- `edge=1` is the first testbench rising edge.
- RTL reset occupies edges 1 through 100.
- The first active CPU edge is edge 101 with `cpu_cycle=1`.
- DONE is sampled from the full-word DBus request to `0x4001e004`, exactly as
  `aerith_tb_top.sv` does. It is not delayed until the DBus response.

## Install or refresh the RTL monitor

The installer is idempotent. Run it again whenever the checked-in monitor is
updated:

```powershell
python util/brs/install_rtl_cycle_trace.py `
  "C:\Users\24103\Desktop\spirit_new+veu\aerith\sim\src\top\aerith_tb_top.sv"
```

The monitor is enabled by the `BRS_TRACE` plusarg. Examples:

```text
vsim ... work.aerith_tb_top +BRS_TRACE=rtl_cycle_trace.log
xsim aerith_tb_top_snapshot -testplusarg BRS_TRACE -runall
```

Vivado 2020.2 uses `rtl_cycle_trace.log` in the xsim working directory for the
flag-only form. Questa accepts the value form and can select another path.

Vivado also rejects the original shared response-pipeline loop variable in
`crossbar.sv`. Apply the functionally neutral compatibility fix once:

```powershell
python util/brs/fix_rtl_xsim_crossbar.py `
  "C:\Users\24103\Desktop\spirit_new+veu\aerith\sim\src\ip\crossbar.sv"
```

When elaborating the generated Spirit files with xsim, specify a global
timescale because most generated modules do not contain a timescale directive:

```text
xelab -timescale 1ns/1ps aerith_tb_top -s aerith_tb_top_snapshot
```

Use the same `inst_mem.hex` and `data_mem.hex` files for both simulators.

## Run gem5

Use the RTL-testbench memory mode and load the same `$readmemh` word images:

```powershell
build\RISCV\gem5.opt --outdir=m5out `
  configs\brs\run_pipeline_mini.py `
  --mem-system rtl-aerith-tb `
  --program-file inst_mem.hex `
  --dmem-hex data_mem.hex `
  --cycle-trace gem5_cycle_trace.log
```

The gem5 trace will be written as `m5out/gem5_cycle_trace.log`. The default RTL
mode timeout counts total rising edges including reset, matching the
testbench.

## Compare

```powershell
python util/brs/compare_cycle_traces.py `
  rtl_cycle_trace.log m5out\gem5_cycle_trace.log `
  --window 12 --json-report cycle_compare.json
```

Exit status is `0` only when DONE, retire timing/writeback, and cycle-level
bus/control signals all match. A mismatch returns `1`; malformed or incomplete
trace input returns `2`.

The authoritative quick regression result is the DONE edge. A matching DONE
edge alone is not certification: the retire and cycle layers must also pass.
