# Mikui SRAM/DMA variant

This directory is the SRAM-staged counterpart of `gem5_cpu_veu16x16`.

It keeps:

- the CPU-connected `sau_mikui` cycle model;
- `rtl-npu-lpnpu-mikui`;
- `rtl-npu-lpnpu-mikui-decompress-dma`;
- `DDR4_2400_8x8 -> MikuiDecompressDma -> shared three-bank SRAM`;
- CPU, VEU and SAU access to the shared Mikui SRAM model.

It intentionally does not contain the exploratory
`rtl-npu-lpnpu-mikui-ddr-demand` mode or `MikuiSauDdrDemand`.

Build and verify from this directory:

```sh
scons build/RISCV/gem5.opt -j8
bash test_by_agent/rv_dma_e2e/run.sh
```
