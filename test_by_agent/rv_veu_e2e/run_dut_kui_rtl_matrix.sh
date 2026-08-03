#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/rv_veu_e2e"
PROFILE="configs/brs/veu_timing_profile.csv"
GENERATOR="$ROOT/gen_veu_e2e_hex.py"
FUNCTIONAL_VERIFIER="$ROOT/verify_veu_e2e.py"
TIMING_VERIFIER="$ROOT/verify_dut_kui_rtl_case.py"
RTL_ROOT="${BRS_VEU_RTL_RESULTS_DIR:-../../veu_timing_results}"
OUTROOT="${BRS_VEU_RTL_OUTROOT:-$ROOT/m5out_dut_kui_rtl_matrix}"
SUMMARY="$OUTROOT/summary.csv"
MAX_CYCLES=10000

if [[ ! -x "$GEM5" ]]; then
    echo "missing gem5 binary: $GEM5" >&2
    exit 2
fi

mkdir -p "$OUTROOT"
printf '%s\n' \
    'rtl_test,op,vlen,config,mask,status,gem5_operation_cycles,rtl_operation_cycles,vfu_latency,vfu_ii,vsu_latency,profile_id,evidence_id,control_timing_source,control_evidence_id,event_status' \
    > "$SUMMARY"

failures=0
total=0
for capture in yinglong_veu_timing_fixed2 yinglong_veu_timing_coverage; do
    vectors="$RTL_ROOT/$capture/veu_functional_vectors.csv"
    rtl_summary="$RTL_ROOT/$capture/veu_timing_summary.csv"
    rtl_events="$RTL_ROOT/$capture/veu_timing_events.csv"
    for required in "$vectors" "$rtl_summary" "$rtl_events"; do
        if [[ ! -f "$required" ]]; then
            echo "missing RTL capture artifact: $required" >&2
            exit 2
        fi
    done

    while IFS= read -r rtl_test; do
        [[ -z "$rtl_test" ]] && continue
        total=$((total + 1))
        case_dir="$OUTROOT/$rtl_test"
        run_log="$case_dir/run.log"
        mkdir -p "$case_dir"

        if ! python3 "$GENERATOR" \
            --rtl-vectors "$vectors" \
            --rtl-test "$rtl_test" \
            --outdir "$case_dir"; then
            echo "$rtl_test,,,,,FAIL_GENERATOR,,,,,,," >> "$SUMMARY"
            failures=$((failures + 1))
            continue
        fi

        BRS_RETIRE_TRACE=1 "$GEM5" -d "$case_dir" "$CONFIG" \
            --mem-system rtl-dut-kui-tb \
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
            echo "$rtl_test,,,,,FAIL_GEM5_${gem5_status},,,,,,," >> "$SUMMARY"
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
            echo "$rtl_test,,,,,FAIL_FUNCTIONAL,,,,,,," >> "$SUMMARY"
            failures=$((failures + 1))
            continue
        fi

        if python3 "$TIMING_VERIFIER" \
            --metadata "$case_dir/metadata.json" \
            --trace "$case_dir/veu_cycle_trace.csv" \
            --stats "$case_dir/stats.txt" \
            --rtl-summary "$rtl_summary" \
            --rtl-events "$rtl_events" \
            --profile "$PROFILE" \
            > "$case_dir/timing_verify.csv" \
            2> "$case_dir/timing_verify.log"; then
            cat "$case_dir/timing_verify.csv" >> "$SUMMARY"
        else
            echo "$rtl_test,,,,,FAIL_TIMING,,,,,,," >> "$SUMMARY"
            failures=$((failures + 1))
        fi
    done < <(
        python3 "$GENERATOR" --rtl-vectors "$vectors" --list-rtl-tests
    )
done

cat "$SUMMARY"
if (( failures != 0 )); then
    echo "dut_kui RTL matrix failed: $failures/$total case(s)" >&2
    exit 1
fi
matches=$(awk -F, 'NR > 1 && $6 == "MATCH" { count++ } END { print count + 0 }' \
    "$SUMMARY")
diffs=$(awk -F, 'NR > 1 && $6 == "DIFF" { count++ } END { print count + 0 }' \
    "$SUMMARY")
echo "dut_kui RTL matrix functional PASS: $total cases; timing MATCH=$matches DIFF=$diffs"
