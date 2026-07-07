#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/spirit_like_smoke"

stat_value() {
    local outdir="$1"
    local stat="$2"
    awk -v stat="$stat" '$1 == stat { print $2; found = 1 } END { exit !found }' \
        "$outdir/stats.txt"
}

assert_stat() {
    local outdir="$1"
    local stat="$2"
    local expected="$3"
    local actual
    actual="$(stat_value "$outdir" "$stat")"
    if [[ "$actual" != "$expected" ]]; then
        echo "FAIL: $outdir $stat expected $expected, got $actual" >&2
        exit 1
    fi
}

run_case() {
    local name="$1"
    local program="$2"
    local max_cycles="$3"
    shift 3

    local outdir="$ROOT/m5out_$name"
    echo "== $name =="
    "$GEM5" -d "$outdir" "$CONFIG" \
        --mem-system spirit-like \
        --program-file "$ROOT/$program" \
        --imem-base 0x0 \
        --dmem-base 0x0 \
        --max-cycles "$max_cycles" \
        "$@"
}

summarize_case() {
    local name="$1"
    local outdir="$ROOT/m5out_$name"
    echo "-- $name stats --"
    grep -E "cycle_count|retired_inst_count|stall_count|flush_count|icache_hit_count|icache_miss_count|ibus_req_count|fetch_fifo_flush_count|aligned_instr_count" \
        "$outdir/stats.txt"
}

run_case baseline_aligned instr_mem.hex 200 \
    --dmem-hex "$ROOT/data_mem.hex"

assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.cycle_count 65
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.retired_inst_count 11
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.flush_count 0
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.icache_hit_count 3
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.icache_miss_count 1
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.ibus_req_count 4
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.fetch_fifo_flush_count 0
assert_stat "$ROOT/m5out_baseline_aligned" system.pipeline.aligned_instr_count 11

run_case redirect_misaligned redirect_misaligned_instr_mem.hex 120

assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.cycle_count 23
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.retired_inst_count 3
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.flush_count 1
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.icache_hit_count 3
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.icache_miss_count 1
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.ibus_req_count 4
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.fetch_fifo_flush_count 1
assert_stat "$ROOT/m5out_redirect_misaligned" system.pipeline.aligned_instr_count 5

run_case redirect_misaligned_no_icache redirect_misaligned_instr_mem.hex 120 \
    --no-icache

assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.cycle_count 43
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.retired_inst_count 3
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.flush_count 1
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.icache_hit_count 0
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.icache_miss_count 4
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.ibus_req_count 4
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.fetch_fifo_flush_count 1
assert_stat "$ROOT/m5out_redirect_misaligned_no_icache" system.pipeline.aligned_instr_count 5

summarize_case baseline_aligned
summarize_case redirect_misaligned
summarize_case redirect_misaligned_no_icache
echo "PASS: spirit-like smoke"
