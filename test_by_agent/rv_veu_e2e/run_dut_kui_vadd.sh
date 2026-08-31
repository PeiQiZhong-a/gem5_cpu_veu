#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/rv_veu_e2e"
PROFILE="configs/brs/veu_timing_profile.csv"
TERMINAL_BEHAVIOR="configs/brs/veu_terminal_behavior.csv"
GENERATOR="$ROOT/gen_veu_e2e_hex.py"
FUNCTIONAL_VERIFIER="$ROOT/verify_veu_e2e.py"
OUTROOT="$ROOT/m5out_dut_kui_vadd"
SUMMARY="$OUTROOT/summary.csv"
MAX_CYCLES=10000

if [[ ! -x "$GEM5" ]]; then
    echo "missing gem5 binary: $GEM5" >&2
    exit 2
fi
mkdir -p "$OUTROOT"
printf '%s\n' \
    'case,op,vlen,status,cycle_count,chunks,reads,writes,max_outstanding,retries,profile_hits,profile_fallbacks,detail' \
    > "$SUMMARY"

failures=0
for vlen in 128 1024; do
    case_dir="$OUTROOT/vadd_vector_${vlen}"
    run_log="$case_dir/run.log"
    mkdir -p "$case_dir"

    if ! python3 "$GENERATOR" --case vadd_vector --vlen "$vlen" \
        --layout mikui --outdir "$case_dir"; then
        echo "vadd_vector,vadd,$vlen,FAIL,,,,,,,,,generator_failed" >> "$SUMMARY"
        failures=$((failures + 1))
        continue
    fi

    BRS_RETIRE_TRACE=1 "$GEM5" -d "$case_dir" "$CONFIG" \
        --mem-system rtl-npu-lpnpu-mikui \
        --veu-model timing \
        --veu-timing-profile "$PROFILE" \
        --veu-terminal-behavior "$TERMINAL_BEHAVIOR" \
        --veu-cycle-trace "$case_dir/veu_cycle_trace.csv" \
        --program-file "$case_dir/instr_mem.hex" \
        --dmem-hex "$case_dir/data_mem.hex" \
        --no-icache \
        --max-cycles "$MAX_CYCLES" \
        > "$run_log" 2>&1
    gem5_status=$?
    if (( gem5_status != 0 )); then
        echo "vadd_vector,vadd,$vlen,FAIL,,,,,,,,,gem5_exit_${gem5_status}" >> "$SUMMARY"
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
        echo "vadd_vector,vadd,$vlen,FAIL,,,,,,,,,functional_verification_failed" >> "$SUMMARY"
        failures=$((failures + 1))
        continue
    fi

    cat "$case_dir/functional_verify.csv" >> "$SUMMARY"
done

cat "$SUMMARY"
if (( failures != 0 )); then
    echo "Mikui VADD failed: $failures case(s)" >&2
    exit 1
fi
echo "Mikui native 128-bit VADD PASS: 2 cases"
