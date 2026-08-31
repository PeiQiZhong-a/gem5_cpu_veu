# Mikui decompression DMA model

## Selected RTL and topology

The gem5 mode `rtl-npu-lpnpu-mikui-decompress-dma` follows the DMA sources
selected by Mikui's active VCS/synthesis file lists:

- `hardware/src/DMA/dma_decompress.v`
- `hardware/src/DMA/dt_dma/dt_dma.v`
- `hardware/src/DMA/decompress/dt_decompress.v`
- `meta_bmp_analyzer.v`, `rice_decode_stream.v`, `zigzag_unmap_uints.v`, and
  `zeros_skip.v`

The mode now selects the `dut_mikui_dma` local-memory topology.  The
instruction SRAM remains independent, while `crossbar_mi_full` routes DBUS,
SAU and VEU traffic to the three default `B/C/D=4/8/12` data partitions:

| Port | Address range | Size |
|---|---|---:|
| stack SRAM | `0x20010000..0x20017fff` | 32 KiB |
| ping SRAM | `0x20018000..0x2001ffff` | 32 KiB |
| pong SRAM | `0x20020000..0x20027fff` | 32 KiB |

The plain `rtl-npu-lpnpu-mikui` mode intentionally retains the older
`dut_mikui.sv` two-bank topology.

`system.mikui_dma` remains an independent `DmaDevice`, rather than a C++
member of `PipelineMiniCPU` or `NpuLpnpuMikuiMemoryModel`. Its connections
mirror the selected decompressor RTL peripheral boundary:

```text
PipelineMiniCPU.data_port -> DMA.pio (AHB slave configuration)
DMA.dma                  -> external SRAM0/SRAM1 (AHB master traffic)
DMA.irq                  -> PipelineMiniCPU.dma_irq (cause 6)
```

The old four-channel `dma_top/dma_reg/dma_master` simulation mode and register
ABI are not available.

## Register and address map

The PIO window is `0x40019c00..0x40019cff`. The implemented `dt_dma.v`
registers are:

| Offset | Register | Meaning |
|---:|---|---|
| `0x00` | CTRL | bit 0 starts an operation; bit 1 is present in RTL but unused |
| `0x04` | SRC | compressed input byte address |
| `0x08` | DST | decompressed output byte address |
| `0x0c` | LENGTH | compressed input size in bytes |

The reference testbench map uses compressed SRAM0 at
`0x60000000..0x60000fff` and output SRAM1 at
`0x60001000..0x60001fff`. Use `--dma-input-image` to preload SRAM0.

## Functional format covered

The model implements the connected RTL data path, including:

- byte-swapped metadata parsing;
- zero-skip block-info and 512-bit per-block bitmap handling;
- MSB-first Rice codes with 3-bit `k` and maximum quotient 15;
- the exact 8-bit zigzag-unmap equation;
- four-element RTL AHB output-word packing;
- up to 1024 blocks, matching the RTL 10-bit block index;
- 32-bit sequential DMA reads and writes through gem5 memory ports.

Malformed, unaligned, oversized, or truncated operations increment
`decodeErrors` and assert IRQ instead of silently corrupting memory.

## Known RTL gaps and timing status

The current Mikui RTL cannot yet be used as a complete timing oracle:

- `top_mikui_dma_tb.sv` declares and maps DMA AHB signals but does not
  instantiate `dma_decompress`;
- `dt_dma.v` does not drive `hrdata_o`;
- `dma_wend` is tied to zero, so the master cannot reach its intended normal
  completion state;
- `dma_stop` is decoded but unused;
- `hardware/src/DMA/tb/dt_decompress_sim.sv` contains no active testbench or
  golden vectors.

The gem5 model is therefore functional and bus-visible, but its internal
decompressor latency is not claimed to be cycle-exact. Memory latency,
crossbar arbitration, 32-bit transfer count, and CPU PIO stalls are timed by
gem5; the C++ decode itself currently has no calibrated RTL delay.

## Data required for cycle calibration

After the RTL integration gaps are fixed, collect VCS traces for dense and
zero-skip streams with multiple `k`, block counts, last-block sizes, bitmap
popcounts, and Rice quotient distributions. For each operation record:

1. the accepted CTRL.START write edge;
2. first/last master read-address handshakes and read-data return edges;
3. first/last `bus_data_valid_o` and `raw_data_valid` edges;
4. first/last master write-address/data handshakes;
5. IRQ assertion/deassertion and the true completed-write edge;
6. every `m_hready_i` stall and AHB-matrix arbitration delay;
7. input/output byte counts and decoded-element count.

These measurements are needed to separate fixed startup/drain latency from
metadata, bitmap, Rice, FIFO-refill, output-packing, and bus-wait costs. They
also decide whether a calibrated aggregate delay is sufficient or whether
gem5 needs a streaming internal state machine that overlaps reads, decode,
and writes like the RTL.
