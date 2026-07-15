#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

GEM5="./build/RISCV/gem5.opt"
CONFIG="configs/brs/run_pipeline_mini.py"
ROOT="test_by_agent/rv_veu_e2e"
OUTDIR="$ROOT/m5out_veu_vadd"
RUN_LOG="$OUTDIR/run.log"
SUMMARY="$OUTDIR/result_summary.txt"

mkdir -p "$OUTDIR"

fail_summary() {
    local reason="$1"
    {
        echo "RV-VEU VADD E2E FAIL"
        echo
        echo "Reason:"
        echo "  $reason"
    } > "$SUMMARY"
    cat "$SUMMARY"
    exit 1
}

stat_value() {
    local stat="$1"
    awk -v stat="$stat" '$1 == stat { print $2; found = 1 } END { exit !found }' \
        "$OUTDIR/stats.txt"
}

trace_data_value() {
    local reg="$1"
    awk -v reg="$reg" '
        $0 ~ ("rd=" reg " ") {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^data=/) {
                    sub(/^data=/, "", $i)
                    value = $i
                }
            }
        }
        END {
            if (value != "") {
                print value
            } else {
                exit 1
            }
        }
    ' "$RUN_LOG"
}

assert_eq() {
    local name="$1"
    local actual="$2"
    local expected="$3"
    if [[ "$actual" != "$expected" ]]; then
        fail_summary "$name expected $expected, got $actual"
    fi
}

assert_ge() {
    local name="$1"
    local actual="$2"
    local expected="$3"
    if (( actual < expected )); then
        fail_summary "$name expected >= $expected, got $actual"
    fi
}

python3 "$ROOT/gen_veu_vadd_hex.py"

BRS_RETIRE_TRACE=1 "$GEM5" -d "$OUTDIR" "$CONFIG" \
    --mem-system spirit-like \
    --veu-model timing \
    --program-file "$ROOT/instr_mem.hex" \
    --dmem-hex "$ROOT/data_mem.hex" \
    --imem-base 0x0 \
    --dmem-base 0x0 \
    --no-icache \
    --max-cycles 300 \
    > "$RUN_LOG" 2>&1

if [[ ! -f "$OUTDIR/stats.txt" ]]; then
    fail_summary "missing $OUTDIR/stats.txt"
fi

if ! lane0_actual="$(trace_data_value x8)"; then
    fail_summary "missing retire trace for rd=x8 in $RUN_LOG"
fi
if ! lane7_actual="$(trace_data_value x9)"; then
    fail_summary "missing retire trace for rd=x9 in $RUN_LOG"
fi

assert_eq "lane0" "$lane0_actual" "0x0000000b"
assert_eq "lane7" "$lane7_actual" "0x00000058"

for stat in \
    system.pipeline.veu_issue_count \
    system.pipeline.veu_complete_count \
    system.pipeline.veu_csr_handshake_cycles \
    system.pipeline.rv_dmem_blocked_by_veu_cycles \
    system.pipeline.veu_operation_start_count \
    system.pipeline.veu_operation_complete_count \
    system.pipeline.veu_busy_cycles \
    system.pipeline.veu_chunks \
    system.pipeline.veu_memory_reads \
    system.pipeline.veu_memory_writes \
    system.pipeline.flush_count
do
    if ! value="$(stat_value "$stat")"; then
        fail_summary "missing stat $stat"
    fi
    case "$stat" in
        system.pipeline.veu_issue_count) assert_ge "$stat" "$value" "6" ;;
        system.pipeline.veu_csr_handshake_cycles) assert_ge "$stat" "$value" "1" ;;
        system.pipeline.veu_operation_start_count) assert_eq "$stat" "$value" "1" ;;
        system.pipeline.veu_operation_complete_count) assert_eq "$stat" "$value" "1" ;;
        system.pipeline.veu_busy_cycles) assert_ge "$stat" "$value" "1" ;;
        system.pipeline.veu_chunks) assert_eq "$stat" "$value" "1" ;;
        system.pipeline.veu_memory_reads) assert_eq "$stat" "$value" "2" ;;
        system.pipeline.veu_memory_writes) assert_eq "$stat" "$value" "1" ;;
        system.pipeline.flush_count) assert_ge "$stat" "$value" "1" ;;
    esac
done

veu_issue_count="$(stat_value system.pipeline.veu_issue_count)"
veu_complete_count="$(stat_value system.pipeline.veu_complete_count)"
assert_eq "VEU issued/completed instructions" \
    "$veu_complete_count" "$veu_issue_count"
veu_csr_handshake_cycles="$(stat_value system.pipeline.veu_csr_handshake_cycles)"
rv_dmem_blocked_by_veu_cycles="$(stat_value system.pipeline.rv_dmem_blocked_by_veu_cycles)"
veu_operation_start_count="$(stat_value system.pipeline.veu_operation_start_count)"
veu_operation_complete_count="$(stat_value system.pipeline.veu_operation_complete_count)"
veu_busy_cycles="$(stat_value system.pipeline.veu_busy_cycles)"
veu_chunks="$(stat_value system.pipeline.veu_chunks)"
veu_memory_reads="$(stat_value system.pipeline.veu_memory_reads)"
veu_memory_writes="$(stat_value system.pipeline.veu_memory_writes)"
cycle_count="$(stat_value system.pipeline.cycle_count)"
stall_count="$(stat_value system.pipeline.stall_count)"
flush_count="$(stat_value system.pipeline.flush_count)"

{
    echo "RV-VEU VADD E2E PASS"
    echo
    echo "Results:"
    echo "  lane0 actual=$lane0_actual expected=0x0000000b"
    echo "  lane7 actual=$lane7_actual expected=0x00000058"
    echo
    echo "Stats:"
    echo "  veu_issue_count=$veu_issue_count"
    echo "  veu_complete_count=$veu_complete_count"
    echo "  veu_csr_handshake_cycles=$veu_csr_handshake_cycles"
    echo "  rv_dmem_blocked_by_veu_cycles=$rv_dmem_blocked_by_veu_cycles"
    echo "  veu_operation_start_count=$veu_operation_start_count"
    echo "  veu_operation_complete_count=$veu_operation_complete_count"
    echo "  veu_busy_cycles=$veu_busy_cycles"
    echo "  veu_chunks=$veu_chunks"
    echo "  veu_memory_reads=$veu_memory_reads"
    echo "  veu_memory_writes=$veu_memory_writes"
    echo "  cycle_count=$cycle_count"
    echo "  stall_count=$stall_count"
    echo "  flush_count=$flush_count"
} > "$SUMMARY"

cat "$SUMMARY"
