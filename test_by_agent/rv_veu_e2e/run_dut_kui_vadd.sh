#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/rv_veu_e2e"
PROFILE="configs/brs/veu_timing_profile.csv"
GENERATOR="$ROOT/gen_veu_e2e_hex.py"
FUNCTIONAL_VERIFIER="$ROOT/verify_veu_e2e.py"
TIMING_VERIFIER="$ROOT/verify_dut_kui_vadd.py"
RTL_SUMMARY="../../veu_timing_results/yinglong_veu_timing_fixed2/veu_timing_summary.csv"
OUTROOT="$ROOT/m5out_dut_kui_vadd"
SUMMARY="$OUTROOT/timing_compare.csv"
MAX_CYCLES=10000

if [[ ! -x "$GEM5" ]]; then
    echo "missing gem5 binary: $GEM5" >&2
    exit 2
fi
if [[ ! -f "$RTL_SUMMARY" ]]; then
    echo "missing RTL timing summary: $RTL_SUMMARY" >&2
    exit 2
fi

mkdir -p "$OUTROOT"
printf '%s\n' \
    'vlen,status,gem5_first_read,rtl_first_read,gem5_read_returns,rtl_read_returns,gem5_first_write,rtl_first_write,gem5_lock_finish_delta,rtl_lock_finish_delta,gem5_total,rtl_total' \
    > "$SUMMARY"

failures=0
for vlen in 256 2048; do
    case_dir="$OUTROOT/vadd_vector_${vlen}"
    run_log="$case_dir/run.log"
    mkdir -p "$case_dir"

    if ! python3 "$GENERATOR" --case vadd_vector --vlen "$vlen" \
        --layout dut-kui --outdir "$case_dir"; then
        echo "$vlen,FAIL,generator_failed" >> "$SUMMARY"
        failures=$((failures + 1))
        continue
    fi

    BRS_RETIRE_TRACE=1 "$GEM5" -d "$case_dir" "$CONFIG" \
        --mem-system rtl-dut-kui-tb \
        --entry-point 0x29110000 \
        --veu-model timing \
        --veu-timing-profile "$PROFILE" \
        --veu-cycle-trace "$case_dir/veu_cycle_trace.csv" \
        --program-file "$case_dir/instr_mem.hex" \
        --dmem-hex "$case_dir/data_mem.hex" \
        --no-icache \
        --max-cycles "$MAX_CYCLES" \
        > "$run_log" 2>&1
    gem5_status=$?
    if (( gem5_status != 0 )); then
        echo "$vlen,FAIL,gem5_exit_${gem5_status}" >> "$SUMMARY"
        failures=$((failures + 1))
        continue
    fi

    if ! python3 "$FUNCTIONAL_VERIFIER" \
        --metadata "$case_dir/metadata.json" \
        --stats "$case_dir/stats.txt" \
        --trace "$case_dir/veu_cycle_trace.csv" \
        --run-log "$run_log" \
        > "$case_dir/functional_verify.csv" \
        2> "$case_dir/functional_verify.log"; then
        echo "$vlen,FAIL,functional_verification_failed" >> "$SUMMARY"
        failures=$((failures + 1))
        continue
    fi

    if python3 "$TIMING_VERIFIER" \
        --metadata "$case_dir/metadata.json" \
        --trace "$case_dir/veu_cycle_trace.csv" \
        --rtl-summary "$RTL_SUMMARY" \
        > "$case_dir/timing_verify.csv" \
        2> "$case_dir/timing_verify.log"; then
        cat "$case_dir/timing_verify.csv" >> "$SUMMARY"
    else
        echo "$vlen,FAIL,timing_verification_failed" >> "$SUMMARY"
        failures=$((failures + 1))
    fi
done

cat "$SUMMARY"
if (( failures != 0 )); then
    echo "dut_kui VADD failed: $failures case(s)" >&2
    exit 1
fi
echo "dut_kui VADD PASS: 2 cases"
