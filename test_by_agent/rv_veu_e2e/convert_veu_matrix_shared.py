#!/usr/bin/env python3
"""Convert the archived VEU matrix into shared gem5/RTL firmware images."""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from pathlib import Path

from gen_veu_e2e_hex import (
    APP_ENTRY,
    APP_IMAGE_BASE,
    CASE_BY_NAME,
    NPU_ERROR_VALUE,
    NPU_STATUS,
    NPU_DONE_VALUE,
    SHARED_RTL_LAYOUT,
    generate,
)


DEFAULT_SOURCE = Path("/home/zpq/文档/m5out_veu_matrix")
DEFAULT_OUTPUT = Path("/home/zpq/文档/m5out_veu_matrix_shared")
GENERATED_FILES = (
    "instr_mem.hex",
    "data_mem.hex",
    "instruction.hex",
    "memory.hex",
    "metadata.json",
    "source_metadata.json",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def discover_cases(source: Path) -> list[tuple[Path, str, int]]:
    cases = []
    for case_dir in sorted(path for path in source.iterdir() if path.is_dir()):
        metadata_path = case_dir / "metadata.json"
        if not metadata_path.is_file():
            continue
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        case_name = str(metadata["case"])
        vlen = int(metadata["vlen"])
        if case_name not in CASE_BY_NAME:
            raise ValueError(f"unsupported case in {metadata_path}: {case_name}")
        expected_dir = f"{case_name}_{vlen}"
        if case_dir.name != expected_dir:
            raise ValueError(
                f"case directory {case_dir.name} does not match {expected_dir}"
            )
        for filename in ("instr_mem.hex", "data_mem.hex"):
            if not (case_dir / filename).is_file():
                raise ValueError(f"missing {case_dir / filename}")
        cases.append((case_dir, case_name, vlen))
    if not cases:
        raise ValueError(f"no VEU cases found under {source}")
    return cases


def validate_source_image(case_dir: Path, case_name: str, vlen: int) -> None:
    """Refuse to silently regenerate a source image with unknown contents."""
    with tempfile.TemporaryDirectory(prefix="veu-source-check-") as tmp:
        check_dir = Path(tmp)
        generate(CASE_BY_NAME[case_name], vlen, check_dir)
        for filename in ("instr_mem.hex", "data_mem.hex"):
            if (case_dir / filename).read_bytes() != (check_dir / filename).read_bytes():
                raise ValueError(
                    f"{case_dir / filename} differs from the current generator; "
                    "conversion would not preserve this source case"
                )


def add_source_provenance(case_dir: Path, output_dir: Path) -> None:
    source_metadata_path = case_dir / "metadata.json"
    source_metadata_text = source_metadata_path.read_text(encoding="utf-8")
    (output_dir / "source_metadata.json").write_text(
        source_metadata_text, encoding="utf-8"
    )

    metadata_path = output_dir / "metadata.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    source_metadata = json.loads(source_metadata_text)
    metadata.update({
        "source_case_dir": str(case_dir),
        "source_instr_mem_sha256": sha256(case_dir / "instr_mem.hex"),
        "source_data_mem_sha256": sha256(case_dir / "data_mem.hex"),
        "source_metadata_sha256": sha256(source_metadata_path),
        "source_expected_timing_source": source_metadata.get(
            "expected_timing_source"
        ),
        "source_expected_control_timing_source": source_metadata.get(
            "expected_control_timing_source"
        ),
    })
    metadata_path.write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )


def write_readme(output: Path, case_count: int) -> None:
    text = f"""# Shared RTL/gem5 VEU matrix

This directory contains {case_count} converted single-operation VEU cases.
Every case has one logical instruction stream and one logical data image,
rendered in both simulator input formats.

- `instr_mem.hex`: gem5, one 32-bit word per line; includes the two-word boot header.
- `data_mem.hex`: gem5, byte tokens in increasing address order.
- `instruction.hex`: RTL QSPI, one packed 128-bit word per line.
- `memory.hex`: RTL QSPI, one packed 128-bit word per line.
- `metadata.json`: relocated addresses, image lengths, completion protocol, and hashes.
- `source_metadata.json`: metadata from the archived source case.

Common mapping:

- image base: `0x{APP_IMAGE_BASE:08x}`
- CPU entry: `0x{APP_ENTRY:08x}`
- data base: `0x{SHARED_RTL_LAYOUT.data_base:08x}`
- completion register: `0x{NPU_STATUS:08x}`
- PASS value: `0x{NPU_DONE_VALUE:x}`
- FAIL value: `0x{NPU_ERROR_VALUE:x}`

RTL example (`vmac_256`):

```sh
cd /home/zpq/下载/npu_lpnpu-origin
make -f sim/vcs/script/case_veu_regress/Makefile sim \\
  FIRMWARE_DIR={output}/vmac_256 \\
  SIM_EXTRA_ARGS="+BTB_OFF=1 +CYCLE_TRACE=/tmp/rtl_vmac_256.trace"
```

gem5 example (`vmac_256`):

```sh
cd /home/zpq/下载/gem5_cpu_veu
build/RISCV/gem5.opt -d m5out/shared_vmac_256 \\
  configs/brs/run_pipeline_mini.py \\
  --mem-system rtl-dut-kui-tb \\
  --entry-point 0x{APP_ENTRY:08x} \\
  --program-file {output}/vmac_256/instr_mem.hex \\
  --dmem-hex {output}/vmac_256/data_mem.hex \\
  --veu-model timing \\
  --veu-timing-profile configs/brs/veu_timing_profile.csv \\
  --cycle-trace gem5_vmac_256.trace \\
  --max-cycles 10000 --no-icache
```

Run all {case_count} gem5 cases automatically:

```sh
cd /home/zpq/下载/gem5_cpu_veu
test_by_agent/rv_veu_e2e/run_shared_matrix.sh \\
  --gem5 /path/to/build/RISCV/gem5.opt \\
  --input-root {output} \\
  --output-root {output}_runs
```

Each output case contains `brs_cycle_trace.log`, `veu_cycle_trace.csv`,
`run.log`, `stats.txt`, and verification logs.  The root `summary.csv`
reports PASS/FAIL for the complete matrix. Use `--case vmac_256` to run only
one case, or `--dry-run` to inspect all commands without starting gem5.
"""
    (output / "README.md").write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--overwrite-generated", action="store_true",
        help="overwrite known generated files in an existing output directory",
    )
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        parser.error(f"source directory does not exist: {source}")
    if output.exists() and any(output.iterdir()) and not args.overwrite_generated:
        parser.error(
            f"output directory is not empty: {output}; use --overwrite-generated"
        )

    cases = discover_cases(source)
    expected = {
        (case_name, vlen)
        for case_name in CASE_BY_NAME
        for vlen in (256, 2048)
    }
    actual = {(case_name, vlen) for _, case_name, vlen in cases}
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(
            f"source is not the complete 44-case matrix; missing={missing}, extra={extra}"
        )

    output.mkdir(parents=True, exist_ok=True)
    manifest_cases = []
    for case_dir, case_name, vlen in cases:
        validate_source_image(case_dir, case_name, vlen)
        converted_dir = output / case_dir.name
        converted_dir.mkdir(parents=True, exist_ok=True)
        generate(
            CASE_BY_NAME[case_name], vlen, converted_dir,
            layout=SHARED_RTL_LAYOUT, shared_rtl=True,
        )
        add_source_provenance(case_dir, converted_dir)
        manifest_cases.append({
            "case": case_name,
            "vlen": vlen,
            "directory": case_dir.name,
            "files": {
                filename: sha256(converted_dir / filename)
                for filename in GENERATED_FILES
            },
        })

    manifest = {
        "schema": "rtl-gem5-shared-veu-matrix-v1",
        "source": str(source),
        "output": str(output),
        "case_count": len(manifest_cases),
        "text_base": APP_IMAGE_BASE,
        "entry_point": APP_ENTRY,
        "data_base": SHARED_RTL_LAYOUT.data_base,
        "completion_address": NPU_STATUS,
        "pass_value": NPU_DONE_VALUE,
        "fail_value": NPU_ERROR_VALUE,
        "cases": manifest_cases,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    write_readme(output, len(manifest_cases))
    print(f"converted {len(manifest_cases)} cases: {source} -> {output}")


if __name__ == "__main__":
    main()
