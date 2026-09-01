# Mikui SAU cycle model

This directory contains the fixed-architecture, cycle-stepped model of the
Mikui SAU and its independent-clock gem5 wrapper. The implementation is frozen
against `mikui-debug-dma` commit
`e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8`: 16 rows, 16 columns, 24-bit
accumulators, three transposers, a 48-entry register file, 128-bit SRAM beats,
and one RTL SRAM-delay cycle. Unsupported architecture parameters fail during
construction.

`MikuiSauCycleModel` owns the current/next state of CSR, scheduler, address,
memory, register/shift, feeder, transposer, 256 PE, output, and writeback
blocks. `MikuiSau` is a `ClockedObject` that supplies timestamped CPU and SRAM
mailboxes, so CPU and SAU clocks may differ without same-Tick visibility.
Normal `PipelineMiniCPU` runs select this model; `--sau-model stub` retains the
lightweight endpoint for older tests.

## Build and test

From the gem5 repository root:

```sh
scons build/RISCV/gem5.opt \
  build/RISCV/sau_mikui/sau_mikui_csr_scheduler.test.opt \
  build/RISCV/sau_mikui/sau_mikui_transposer.test.opt \
  build/RISCV/sau_mikui/sau_mikui_pe_array.test.opt \
  build/RISCV/sau_mikui/sau_mikui_memory_feeder.test.opt \
  build/RISCV/sau_mikui/sau_mikui_cycle_model.test.opt -j32

for test in build/RISCV/sau_mikui/*.test.opt; do
  "$test" --gtest_brief=1
done
```

The end-to-end smoke program writes all four command slots, starts a GEMM,
polls the busy bit until completion, and exercises shared three-bank SRAM. Run
it at the same frequency and at CPU:SAU=1:2 with:

```sh
build/RISCV/gem5.opt -d /tmp/mikui-sau-same \
  configs/brs/run_pipeline_mini.py \
  --mem-system rtl-npu-lpnpu-mikui \
  --program-file tests/brs/mikui_sau_smoke.hex \
  --clock-frequency 100MHz --sau-clock-frequency 100MHz \
  --max-cycles 3000

build/RISCV/gem5.opt -d /tmp/mikui-sau-half \
  configs/brs/run_pipeline_mini.py \
  --mem-system rtl-npu-lpnpu-mikui \
  --program-file tests/brs/mikui_sau_smoke.hex \
  --clock-frequency 100MHz --sau-clock-frequency 50MHz \
  --max-cycles 3000
```

Use `--sau-cycle-trace <name>.csv` to write the structural trace into the gem5
output directory. Trace output is disabled by default. The registered
`mikuiSau` statistics group reports commands by mode, scheduler and module
activity cycles, SRAM beats, transposer errors, and strict timing errors.

## RTL comparison status

The cycle trace is intended for comparison with a matching RTL FSDB through
`/home/xch/work/npi_fsdb_probe`. The current RTL checkout has no `testcase/`
directory, and the available `mikui_original.fsdb` was produced from a
different RTL revision whose SAU sources have different hashes. It must not be
used as cycle-acceptance evidence for the frozen baseline above. Generate or
provide a case from the frozen commit before declaring the full RTL waveform
check complete.

The current array engine advances all 256 PE states, but GEMM/convolution
writeback is still selected from the engine's parallel result matrices. A
trial switch to PE-registered results exposed an unresolved RETAIN command
boundary: the next command can update before the previous PE wavefront reaches
the RTL `OS_valid` boundary. Therefore this checkout is a tested functional
model and integration baseline, not yet the plan's completed PE-sourced,
cycle-matched model. A matching RTL testcase/FSDB is required to freeze that
boundary before removing the parallel result path.
