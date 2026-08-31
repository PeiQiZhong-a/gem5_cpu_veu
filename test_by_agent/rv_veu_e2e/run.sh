#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/rv_veu_e2e"
PROFILE="configs/brs/veu_timing_profile.csv"
TERMINAL_BEHAVIOR="configs/brs/veu_terminal_behavior.csv"
GENERATOR="$ROOT/gen_veu_e2e_hex.py"
VERIFIER="$ROOT/verify_veu_e2e.py"
OUTROOT="$ROOT/m5out_veu_matrix"
SUMMARY="$OUTROOT/summary.csv"
MAX_CYCLES=10000

if [[ ! -x "$GEM5" ]]; then
    echo "missing gem5 binary: $GEM5" >&2
    exit 2
fi
if [[ ! -f "$PROFILE" ]]; then
    echo "missing timing profile: $PROFILE" >&2
    exit 2
fi
if [[ ! -f "$TERMINAL_BEHAVIOR" ]]; then
    echo "missing terminal behavior: $TERMINAL_BEHAVIOR" >&2
    exit 2
fi

mkdir -p "$OUTROOT"
printf '%s\n' \
    'case,op,vlen,status,cycle_count,chunks,reads,writes,max_outstanding,retries,profile_hits,profile_fallbacks,detail' \
    > "$SUMMARY"

mapfile -t CASES < <(python3 "$GENERATOR" --list-cases)
failures=0

append_failure() {
    local case_name="$1"
    local op_name="$2"
    local vlen="$3"
    local detail="$4"
    printf '%s,%s,%s,FAIL,,,,,,,,,%s\n' \
        "$case_name" "$op_name" "$vlen" "$detail" >> "$SUMMARY"
}

for vlen in 128 1024; do
    for case_name in "${CASES[@]}"; do
        case_dir="$OUTROOT/${case_name}_${vlen}"
        run_log="$case_dir/run.log"
        verify_row="$case_dir/verify_row.csv"
        verify_log="$case_dir/verify.log"
        mkdir -p "$case_dir"

        if ! python3 "$GENERATOR" --case "$case_name" --vlen "$vlen" \
            --outdir "$case_dir"; then
            append_failure "$case_name" unknown "$vlen" generator_failed
            failures=$((failures + 1))
            continue
        fi

        BRS_RETIRE_TRACE=1 "$GEM5" -d "$case_dir" "$CONFIG" \
            --mem-system spirit-like \
            --veu-model timing \
            --veu-timing-profile "$PROFILE" \
            --veu-terminal-behavior "$TERMINAL_BEHAVIOR" \
            --veu-cycle-trace "$case_dir/veu_cycle_trace.csv" \
            --program-file "$case_dir/instr_mem.hex" \
            --dmem-hex "$case_dir/data_mem.hex" \
            --imem-base 0x0 \
            --dmem-base 0x0 \
            --no-icache \
            --max-cycles "$MAX_CYCLES" \
            > "$run_log" 2>&1
        gem5_status=$?
        if (( gem5_status != 0 )); then
            append_failure "$case_name" unknown "$vlen" "gem5_exit_${gem5_status}"
            failures=$((failures + 1))
            continue
        fi

        if python3 "$VERIFIER" \
            --metadata "$case_dir/metadata.json" \
            --stats "$case_dir/stats.txt" \
            --trace "$case_dir/veu_cycle_trace.csv" \
            --run-log "$run_log" \
            > "$verify_row" 2> "$verify_log"; then
            cat "$verify_row" >> "$SUMMARY"
        else
            operation="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["op"])' "$case_dir/metadata.json")"
            append_failure "$case_name" "$operation" "$vlen" verification_failed
            failures=$((failures + 1))
        fi
    done
done

cat "$SUMMARY"
if (( failures != 0 )); then
    echo "RV-VEU E2E matrix failed: $failures case(s)" >&2
    exit 1
fi
echo "RV-VEU E2E matrix PASS: $((${#CASES[@]} * 2)) cases"
