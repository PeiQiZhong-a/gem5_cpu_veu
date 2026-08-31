# Mikui decompression DMA end-to-end test

This test uses `rtl-npu-lpnpu-mikui-decompress-dma`. The RV32 guest programs
the four registers implemented by `dt_dma.v`; the independent gem5 DMA reads
an eight-byte compressed stream from external SRAM at `0x60000000`, performs
the VCS-selected Rice/zigzag/zero-skip decode, and writes one 32-bit word to
external SRAM at `0x60001000`.

Run from the gem5 repository root:

```sh
bash test_by_agent/rv_dma_e2e/run.sh
```

The verifier checks that the guest reaches EBREAK, that the DMA performs two
32-bit reads and one 32-bit write, and that the output checksum corresponds to
decoded values `0, -1, 1, -2` with RTL bus-word packing.

The complete register/format contract and RTL timing limitations are recorded
in `docs/MIKUI_DECOMPRESS_DMA.md`.
