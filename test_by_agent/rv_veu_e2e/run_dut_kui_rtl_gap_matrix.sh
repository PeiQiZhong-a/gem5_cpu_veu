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
RTL_ROOT="${BRS_VEU_RTL_GAP_DIR:-../../rtl_test_case}"
OUTROOT="${BRS_VEU_RTL_GAP_OUTROOT:-$ROOT/m5out_dut_kui_rtl_gap_matrix}"
SUMMARY="$OUTROOT/summary.csv"
VECTORS="$RTL_ROOT/veu_functional_vectors.csv"
RTL_SUMMARY="$RTL_ROOT/veu_timing_summary.csv"
RTL_EVENTS="$RTL_ROOT/veu_timing_events.csv"
HANDOFF="$RTL_ROOT/VEU_RTL_MEASURED_AI_HANDOFF.json"
MAX_CYCLES=10000

for required in \
    "$GEM5" "$PROFILE" "$VECTORS" "$RTL_SUMMARY" "$RTL_EVENTS" "$HANDOFF"; do
    if [[ ! -e "$required" ]]; then
        echo "missing VEU RTL-gap artifact: $required" >&2
        exit 2
    fi
done

mkdir -p "$OUTROOT"
printf '%s\n' \
    'rtl_test,op,vlen,config,mask,status,gem5_operation_cycles,rtl_operation_cycles,vfu_latency,vfu_ii,vsu_latency,profile_id,evidence_id,control_timing_source,control_evidence_id,event_status,read_candidate_cycles,reads_blocked_by_store_cycles,vfu_max_in_flight,vsu_queue_stall_cycles' \
    > "$SUMMARY"

failures=0
total=0
while IFS= read -r rtl_test; do
    [[ -z "$rtl_test" ]] && continue
    total=$((total + 1))
    case_dir="$OUTROOT/$rtl_test"
    run_log="$case_dir/run.log"
    mkdir -p "$case_dir"

    if ! python3 "$GENERATOR" \
        --rtl-vectors "$VECTORS" \
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
        --rtl-summary "$RTL_SUMMARY" \
        --rtl-events "$RTL_EVENTS" \
        --profile "$PROFILE" \
        --handoff-manifest "$HANDOFF" \
        > "$case_dir/timing_verify.csv" \
        2> "$case_dir/timing_verify.log"; then
        cat "$case_dir/timing_verify.csv" >> "$SUMMARY"
    else
        echo "$rtl_test,,,,,FAIL_TIMING,,,,,,," >> "$SUMMARY"
        failures=$((failures + 1))
    fi
done < <(
    python3 "$GENERATOR" --rtl-vectors "$VECTORS" --list-rtl-tests
)

cat "$SUMMARY"
if (( failures != 0 )); then
    echo "dut_kui RTL-gap matrix failed: $failures/$total case(s)" >&2
    exit 1
fi
matches=$(awk -F, 'NR > 1 && $6 == "MATCH" { count++ } END { print count + 0 }' \
    "$SUMMARY")
events=$(awk -F, 'NR > 1 && $16 == "EVENT_MATCH" { count++ } END { print count + 0 }' \
    "$SUMMARY")
echo "dut_kui RTL-gap matrix PASS: functional=$total/$total timing=$matches/$total events=$events/$total"
