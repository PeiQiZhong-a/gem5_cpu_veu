#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
fixture_root=${FIXTURE_ROOT:-"$repo_root/fixtures/conv3_step1"}
archive=${ARCHIVE:-/home/xch/workspace/kuiloong-conv2d-testcase2.tar.gz}
gem5=${GEM5:-"$repo_root/build/RISCV/gem5.opt"}
config=${CONFIG:-"$repo_root/configs/brs/run_pipeline_mini.py"}
output_root=${OUT_ROOT:-"$script_dir/runs"}
max_cycles=${MAX_CYCLES:-2000000}
reset_cycles=${RESET_CYCLES:-10}
clock_frequency=${CLOCK_FREQUENCY:-100MHz}

verifier="$script_dir/verify_conv3_e2e.py"
fixture_verifier="$fixture_root/verify_fixtures.py"

if [[ ! -x "$gem5" ]]; then
    echo "missing executable gem5: $gem5" >&2
    echo "build the current branch first, then rerun this script" >&2
    exit 2
fi
if [[ ! -f "$archive" ]]; then
    echo "missing immutable archive: $archive" >&2
    exit 2
fi

python3 "$fixture_verifier" --root "$fixture_root" --archive "$archive"
mkdir -p "$output_root"

archive_fixture="$output_root/archive_baseline_fixture"
mkdir -p "$archive_fixture"
tar -xzf "$archive" -C "$archive_fixture" instruction.hex memory.hex
cp "$script_dir/archive_baseline_manifest.json" "$archive_fixture/manifest.json"

run_case() {
    local case_name=$1
    local sau_model=$2
    local fixture_dir=$3
    local run_dir="$output_root/${case_name}-${sau_model}"
    local memory_args=()

    if [[ "$fixture_dir" == "$archive_fixture" ]]; then
        # The immutable archive declares the original 4 MiB data window.  It
        # is still the same dut_kui timing model; only its decoded geometry
        # is selected to match the archive's address contract.
        memory_args=(
            --rtl-data-base 0x20000000
            --rtl-data-size 0x00400000
            --rtl-data-bank-size 0x00100000
            --rtl-data-bank-count 4
            --rtl-data-real-bank-count 4
        )
    fi

    mkdir -p "$run_dir"
    echo "running $case_name + $sau_model"
    "$gem5" -d "$run_dir" "$config" \
        --mem-system rtl-dut-kui-tb \
        --clock-frequency "$clock_frequency" \
        --reset-cycles "$reset_cycles" \
        --max-cycles "$max_cycles" \
        --no-icache \
        --cycle-trace-compact \
        --terminate-on-ebreak \
        --veu-model fake \
        --sau-model "$sau_model" \
        "${memory_args[@]}" \
        --program-file "$fixture_dir/instruction.hex" \
        --dmem-hex "$fixture_dir/memory.hex" \
        --cycle-trace "$run_dir/cycle_trace.log" \
        >"$run_dir/stdout.log" 2>"$run_dir/stderr.log"

    python3 "$verifier" \
        --fixture-dir "$fixture_dir" \
        --instruction-hex "$fixture_dir/instruction.hex" \
        --stats "$run_dir/stats.txt" \
        --cycle-trace "$run_dir/cycle_trace.log" \
        --sau-model "$sau_model" \
        --json-report "$run_dir/report.json"
}

run_case archive_baseline stub "$archive_fixture"
run_case generated_legacy_control stub \
    "$fixture_root/generated_legacy_control"
run_case generated_four_ins_control_matched stub \
    "$fixture_root/generated_four_ins_control_matched"
run_case generated_four_ins_control_matched sau_n \
    "$fixture_root/generated_four_ins_control_matched"
run_case generated_four_ins_full_offload sau_n \
    "$fixture_root/generated_four_ins_full_offload"

python3 "$script_dir/summarize_conv3_runs.py" \
    --output-root "$output_root" \
    --fixture-root "$fixture_root" \
    --json-report "$output_root/comparison_report.json"

echo "all five supported stub/sau_n comparison runs passed; reports are under $output_root"
