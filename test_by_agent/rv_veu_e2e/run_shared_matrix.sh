#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

GEM5="${BRS_GEM5:-$REPO_ROOT/build/RISCV/gem5.opt}"
CONFIG="$REPO_ROOT/configs/brs/run_pipeline_mini.py"
PROFILE="${BRS_VEU_PROFILE:-$REPO_ROOT/configs/brs/veu_timing_profile.csv}"
INPUT_ROOT="${BRS_SHARED_MATRIX_ROOT:-/home/zpq/文档/m5out_veu_matrix_shared}"
OUTROOT="${BRS_SHARED_RUN_OUTROOT:-/home/zpq/文档/m5out_veu_matrix_shared_runs}"
VERIFIER="$SCRIPT_DIR/verify_veu_e2e.py"
MAX_CYCLES="${BRS_MAX_CYCLES:-10000}"
DRY_RUN=0
declare -a SELECTED_CASES=()

usage() {
    cat <<'EOF'
Usage: run_shared_matrix.sh [options]

Run every converted shared-address VEU case and emit one CPU/VEU trace pair.

Options:
  --gem5 PATH          gem5.opt to execute
  --input-root DIR     converted matrix containing manifest.json
  --output-root DIR    per-case gem5 output and summary directory
  --profile FILE       VEU timing profile CSV
  --max-cycles N       per-case timeout (default: 10000)
  --case NAME          run only this case directory; may be repeated
  --dry-run            print commands without running gem5
  -h, --help           show this help

The same settings can be supplied through BRS_GEM5, BRS_SHARED_MATRIX_ROOT,
BRS_SHARED_RUN_OUTROOT, BRS_VEU_PROFILE, and BRS_MAX_CYCLES.
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --gem5)
            [[ $# -ge 2 ]] || { echo "--gem5 requires a path" >&2; exit 2; }
            GEM5="$2"
            shift 2
            ;;
        --input-root)
            [[ $# -ge 2 ]] || { echo "--input-root requires a directory" >&2; exit 2; }
            INPUT_ROOT="$2"
            shift 2
            ;;
        --output-root)
            [[ $# -ge 2 ]] || { echo "--output-root requires a directory" >&2; exit 2; }
            OUTROOT="$2"
            shift 2
            ;;
        --profile)
            [[ $# -ge 2 ]] || { echo "--profile requires a file" >&2; exit 2; }
            PROFILE="$2"
            shift 2
            ;;
        --max-cycles)
            [[ $# -ge 2 ]] || { echo "--max-cycles requires a value" >&2; exit 2; }
            MAX_CYCLES="$2"
            shift 2
            ;;
        --case)
            [[ $# -ge 2 ]] || { echo "--case requires a case directory name" >&2; exit 2; }
            SELECTED_CASES+=("$2")
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "$MAX_CYCLES" =~ ^[1-9][0-9]*$ ]]; then
    echo "--max-cycles must be a positive integer: $MAX_CYCLES" >&2
    exit 2
fi
for required in "$INPUT_ROOT/manifest.json" "$PROFILE" "$CONFIG" "$VERIFIER"; do
    if [[ ! -f "$required" ]]; then
        echo "missing required file: $required" >&2
        exit 2
    fi
done
if (( ! DRY_RUN )) && [[ ! -x "$GEM5" ]]; then
    echo "missing executable gem5 binary: $GEM5" >&2
    echo "use --gem5 /path/to/build/RISCV/gem5.opt or set BRS_GEM5" >&2
    exit 2
fi

mapfile -t MANIFEST_CASES < <(
    python3 - "$INPUT_ROOT/manifest.json" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1], encoding="utf-8"))
if manifest.get("schema") != "rtl-gem5-shared-veu-matrix-v1":
    raise SystemExit("unsupported shared matrix manifest schema")
for case in manifest["cases"]:
    print(case["directory"])
PY
)
if (( ${#MANIFEST_CASES[@]} == 0 )); then
    echo "manifest contains no runnable cases: $INPUT_ROOT/manifest.json" >&2
    exit 2
fi

if (( ${#SELECTED_CASES[@]} > 0 )); then
    declare -A AVAILABLE=()
    for case_name in "${MANIFEST_CASES[@]}"; do
        AVAILABLE["$case_name"]=1
    done
    for case_name in "${SELECTED_CASES[@]}"; do
        if [[ -z "${AVAILABLE[$case_name]+x}" ]]; then
            echo "case is not present in manifest: $case_name" >&2
            exit 2
        fi
    done
    CASES=("${SELECTED_CASES[@]}")
else
    CASES=("${MANIFEST_CASES[@]}")
fi

mkdir -p "$OUTROOT"
OUTROOT="$(cd "$OUTROOT" && pwd)"
INPUT_ROOT="$(cd "$INPUT_ROOT" && pwd)"
SUMMARY="$OUTROOT/summary.csv"
printf '%s\n' \
    'case,op,vlen,status,cycle_count,chunks,reads,writes,max_outstanding,retries,profile_hits,profile_fallbacks,timing_source,evidence_id,control_timing_source,control_evidence_id,detail' \
    > "$SUMMARY"

append_failure() {
    python3 - "$SUMMARY" "$1" "$2" "$3" "$4" <<'PY'
import csv, sys
with open(sys.argv[1], "a", newline="", encoding="utf-8") as output:
    csv.writer(output).writerow(
        [sys.argv[2], sys.argv[3], sys.argv[4], "FAIL"] + [""] * 12 + [sys.argv[5]]
    )
PY
}

case_metadata_field() {
    python3 - "$1" "$2" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))[sys.argv[2]])
PY
}

failures=0
passed=0
total=${#CASES[@]}
index=0
for case_name in "${CASES[@]}"; do
    index=$((index + 1))
    input_dir="$INPUT_ROOT/$case_name"
    metadata="$input_dir/metadata.json"
    instr="$input_dir/instr_mem.hex"
    data="$input_dir/data_mem.hex"
    rtl_instr="$input_dir/instruction.hex"
    rtl_data="$input_dir/memory.hex"
    case_out="$OUTROOT/$case_name"
    mkdir -p "$case_out"

    op="unknown"
    vlen="unknown"
    if [[ -f "$metadata" ]]; then
        op="$(case_metadata_field "$metadata" op)"
        vlen="$(case_metadata_field "$metadata" vlen)"
    fi
    missing=0
    for required in "$metadata" "$instr" "$data" "$rtl_instr" "$rtl_data"; do
        if [[ ! -f "$required" ]]; then
            echo "[$case_name] missing input: $required" >&2
            missing=1
        fi
    done
    if (( missing )); then
        append_failure "$case_name" "$op" "$vlen" missing_input
        failures=$((failures + 1))
        continue
    fi

    cpu_trace="$case_out/brs_cycle_trace.log"
    veu_trace="$case_out/veu_cycle_trace.csv"
    run_log="$case_out/run.log"
    verify_row="$case_out/verify_row.csv"
    verify_log="$case_out/verify.log"
    command_file="$case_out/command.sh"
    command=(
        "$GEM5" -d "$case_out" "$CONFIG"
        --mem-system rtl-dut-kui-tb
        --entry-point 0x29110008
        --veu-model timing
        --veu-timing-profile "$PROFILE"
        --cycle-trace "$cpu_trace"
        --veu-cycle-trace "$veu_trace"
        --program-file "$instr"
        --dmem-hex "$data"
        --no-icache
        --max-cycles "$MAX_CYCLES"
    )
    printf 'BRS_RETIRE_TRACE=1 ' > "$command_file"
    printf '%q ' "${command[@]}" >> "$command_file"
    printf '\n' >> "$command_file"

    if (( DRY_RUN )); then
        printf '[DRY-RUN %s] ' "$case_name"
        printf '%q ' "${command[@]}"
        printf '\n'
        continue
    fi

    : > "$cpu_trace"
    : > "$veu_trace"
    : > "$case_out/stats.txt"
    : > "$verify_row"
    : > "$verify_log"
    echo "[$index/$total] running $case_name"
    BRS_RETIRE_TRACE=1 "${command[@]}" > "$run_log" 2>&1
    gem5_status=$?
    if (( gem5_status != 0 )); then
        append_failure "$case_name" "$op" "$vlen" "gem5_exit_$gem5_status"
        failures=$((failures + 1))
        echo "[$case_name] FAIL: gem5 exit $gem5_status" >&2
        continue
    fi

    if [[ ! -s "$cpu_trace" ]] || \
       ! head -n 1 "$cpu_trace" | grep -q '^# brs-cycle-trace-v3 source=gem5 '; then
        append_failure "$case_name" "$op" "$vlen" invalid_cpu_trace
        failures=$((failures + 1))
        echo "[$case_name] FAIL: missing/invalid CPU cycle trace" >&2
        continue
    fi
    if ! grep -Eq '(^| )done=1( |$)' "$cpu_trace"; then
        append_failure "$case_name" "$op" "$vlen" missing_done_store
        failures=$((failures + 1))
        echo "[$case_name] FAIL: CPU trace has no done=1 status write" >&2
        continue
    fi
    if grep -Eq '(^| )error=1( |$)' "$cpu_trace"; then
        append_failure "$case_name" "$op" "$vlen" unexpected_error_store
        failures=$((failures + 1))
        echo "[$case_name] FAIL: CPU trace contains error=1" >&2
        continue
    fi

    if python3 "$VERIFIER" \
        --metadata "$metadata" \
        --stats "$case_out/stats.txt" \
        --trace "$veu_trace" \
        --run-log "$run_log" \
        > "$verify_row" 2> "$verify_log"; then
        cat "$verify_row" >> "$SUMMARY"
        passed=$((passed + 1))
        echo "[$case_name] PASS"
    else
        append_failure "$case_name" "$op" "$vlen" verification_failed
        failures=$((failures + 1))
        echo "[$case_name] FAIL: see $verify_log" >&2
    fi
done

if (( DRY_RUN )); then
    echo "dry run complete: ${#CASES[@]} case(s)"
    exit 0
fi

echo "summary: $SUMMARY"
echo "result: PASS=$passed FAIL=$failures TOTAL=$total"
if (( failures != 0 )); then
    exit 1
fi
exit 0
