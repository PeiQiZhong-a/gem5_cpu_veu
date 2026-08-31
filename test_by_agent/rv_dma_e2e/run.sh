#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

ROOT="test_by_agent/rv_dma_e2e"
OUT="$ROOT/m5out"
python3 "$ROOT/gen_dma_e2e_hex.py"
mkdir -p "$OUT"

./build/RISCV/gem5.opt -d "$OUT" configs/brs/run_pipeline_mini.py \
    --mem-system rtl-npu-lpnpu-mikui-decompress-dma \
    --program-file "$ROOT/instr_mem.hex" \
    --dma-input-image "$ROOT/dma_input.bin" \
    --no-icache \
    --terminate-on-ebreak \
    --max-cycles 4000 \
    > "$OUT/run.log" 2>&1

python3 "$ROOT/verify_dma_e2e.py" \
    --run-log "$OUT/run.log" \
    --stats "$OUT/stats.txt"
