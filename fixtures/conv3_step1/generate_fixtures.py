#!/usr/bin/env python3
"""Build the Step 1 RV32 comparison fixtures.

The source files are assembled with the pinned LLVM tools, converted to a
raw .text binary with llvm-objcopy, and then rendered as the same one-word
32-bit little-endian readmemh format used by the RTL testbench. The original
archive memory.hex is copied byte-for-byte into each fixture; the archive is
never modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tarfile
import tempfile
from pathlib import Path


WORD_COUNT = 65536
WORD_BYTES = 4
IMAGE_BYTES = WORD_COUNT * WORD_BYTES
DATA_BASE = 0x29120000
DATA_CAPACITY = 0x30000

FIXTURE_ROOT = Path(__file__).resolve().parent
COMMON_SOURCE = FIXTURE_ROOT / "conv3_fixture_common.S"

FIXTURES = {
    "generated_legacy_control": {
        "source": FIXTURE_ROOT / "generated_legacy_control.S",
        "mode": "legacy_control",
        "sau_counts": {"msetins1": 1, "msetins2": 1,
                       "msetins3": 32, "msetins4": 32},
        "software_padding": True,
        "purpose": "same-shape legacy control with repeated tile starts",
    },
    "generated_four_ins_control_matched": {
        "source": FIXTURE_ROOT / "generated_four_ins_control_matched.S",
        "mode": "four_ins_control_matched",
        "sau_counts": {"msetins1": 1, "msetins2": 1,
                       "msetins3": 1, "msetins4": 1},
        "software_padding": True,
        "purpose": "control-only ABI comparison on StubSau",
    },
    "generated_four_ins_full_offload": {
        "source": FIXTURE_ROOT / "generated_four_ins_full_offload.S",
        "mode": "four_ins_full_offload",
        "sau_counts": {"msetins1": 1, "msetins2": 1,
                       "msetins3": 1, "msetins4": 1},
        "software_padding": False,
        "purpose": "sau_n endpoint full-offload e2e",
    },
}

SAU_FUNCT7_TO_SLOT = {0: 1, 3: 2, 6: 3, 9: 4}
EXPECTED_ARCHIVE_SITES = [
    {"pc": 0x09CA, "word": 0x00AB906B, "slot": 1,
     "rs1": 23, "rs2": 10},
    {"pc": 0x0A08, "word": 0x06C5906B, "slot": 2,
     "rs1": 11, "rs2": 12},
    {"pc": 0x0B40, "word": 0x0D5D106B, "slot": 3,
     "rs1": 26, "rs2": 21},
    {"pc": 0x0B46, "word": 0x135B906B, "slot": 4,
     "rs1": 23, "rs2": 21},
    {"pc": 0x0B64, "word": 0x0C53906B, "slot": 3,
     "rs1": 7, "rs2": 5},
    {"pc": 0x0B7A, "word": 0x1272906B, "slot": 4,
     "rs1": 5, "rs2": 7},
]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def first_version_line(tool: str) -> str:
    result = subprocess.run(
        [tool, "--version"], check=True, capture_output=True, text=True
    )
    return result.stdout.splitlines()[0].strip()


def archive_member(archive: tarfile.TarFile, name: str) -> bytes:
    member = archive.extractfile(name)
    if member is None:
        raise RuntimeError(f"archive member is missing: {name}")
    return member.read()


def parse_word_tokens(data: bytes, label: str) -> list[int]:
    tokens = data.split()
    if len(tokens) != WORD_COUNT:
        raise RuntimeError(
            f"{label} has {len(tokens)} tokens; expected {WORD_COUNT}"
        )
    words = []
    for index, token in enumerate(tokens):
        try:
            text = token.decode("ascii")
            value = int(text, 16)
        except (UnicodeDecodeError, ValueError) as error:
            raise RuntimeError(f"invalid {label} token {index}: {token!r}") from error
        if len(text) != 8 or value > 0xFFFFFFFF:
            raise RuntimeError(f"invalid 32-bit {label} token {index}: {text!r}")
        words.append(value)
    return words


def archive_sau_sites(instruction_hex: bytes) -> list[dict[str, int]]:
    words = parse_word_tokens(instruction_hex, "archive instruction.hex")
    image = b"".join(word.to_bytes(4, "little") for word in words)
    sites = []
    # RV32 instructions may start at any 2-byte boundary when C is enabled.
    for pc in range(0, len(image) - 3, 2):
        word = int.from_bytes(image[pc:pc + 4], "little")
        if (word & 0x7F) != 0x6B or ((word >> 12) & 0x7) != 1:
            continue
        funct7 = (word >> 25) & 0x7F
        if funct7 not in (0, 3, 6, 9):
            continue
        sites.append({
            "pc": pc,
            "word": word,
            "slot": SAU_FUNCT7_TO_SLOT[funct7],
            "rs1": (word >> 15) & 0x1F,
            "rs2": (word >> 20) & 0x1F,
        })
    return sites


def archive_control_flow_observations(instruction_hex: bytes) -> dict[str, object]:
    words = parse_word_tokens(instruction_hex, "archive instruction.hex")
    image = b"".join(word.to_bytes(4, "little") for word in words)
    compressed_ebreak_pcs = []
    uncompressed_ebreak_pcs = []
    for pc in range(0, len(image) - 1, 2):
        halfword = int.from_bytes(image[pc:pc + 2], "little")
        if halfword == 0x9002:
            compressed_ebreak_pcs.append(pc)
    for pc in range(0, len(image) - 3, 2):
        word = int.from_bytes(image[pc:pc + 4], "little")
        if word == 0x00100073:
            uncompressed_ebreak_pcs.append(pc)
    return {
        "static_compressed_ebreak_pcs": compressed_ebreak_pcs,
        "static_uncompressed_ebreak_pcs": uncompressed_ebreak_pcs,
        "retire_trace_available": False,
        "dynamic_sau_count_basis": (
            "archive source/static program analysis; no retire trace was supplied"
        ),
    }


def render_instruction_hex(binary: bytes) -> bytes:
    if len(binary) > IMAGE_BYTES:
        raise RuntimeError(
            f"assembled .text is {len(binary)} bytes; image capacity is {IMAGE_BYTES}"
        )
    padded = binary + bytes(IMAGE_BYTES - len(binary))
    words = [
        int.from_bytes(padded[offset:offset + WORD_BYTES], "little")
        for offset in range(0, IMAGE_BYTES, WORD_BYTES)
    ]
    return ("\n".join(f"{word:08x}" for word in words) + "\n").encode("ascii")


def generated_sau_counts(instruction_hex: bytes) -> dict[str, int]:
    words = parse_word_tokens(instruction_hex, "generated instruction.hex")
    counts = {f"msetins{slot}": 0 for slot in range(1, 5)}
    for word in words:
        if (word & 0x7F) != 0x6B or ((word >> 12) & 0x7) != 1:
            continue
        slot = SAU_FUNCT7_TO_SLOT.get((word >> 25) & 0x7F)
        if slot is not None:
            counts[f"msetins{slot}"] += 1
    return counts


def write_manifest(path: Path, manifest: dict) -> None:
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--archive",
        type=Path,
        default=Path("/home/xch/workspace/kuiloong-conv2d-testcase2.tar.gz"),
        help="immutable baseline archive containing instruction.hex/memory.hex",
    )
    parser.add_argument(
        "--output-root", type=Path, default=FIXTURE_ROOT,
        help="directory containing the three generated fixture directories",
    )
    parser.add_argument("--llvm-mc", default="/usr/bin/llvm-mc")
    parser.add_argument("--llvm-objcopy", default="/usr/bin/llvm-objcopy")
    args = parser.parse_args()

    archive_path = args.archive.resolve()
    output_root = args.output_root.resolve()
    if not archive_path.is_file():
        raise SystemExit(f"archive does not exist: {archive_path}")
    if not COMMON_SOURCE.is_file():
        raise SystemExit(f"common assembly source does not exist: {COMMON_SOURCE}")

    archive_sha = sha256_bytes(archive_path.read_bytes())
    mc_version = first_version_line(args.llvm_mc)
    objcopy_version = first_version_line(args.llvm_objcopy)

    with tarfile.open(archive_path, "r:gz") as archive:
        archive_instruction = archive_member(archive, "instruction.hex")
        archive_memory = archive_member(archive, "memory.hex")
        archive_sources = {
            name: archive_member(archive, name)
            for name in ("main.cpp", "conv2d_no_tile.hpp")
        }

    parse_word_tokens(archive_instruction, "archive instruction.hex")
    parse_word_tokens(archive_memory, "archive memory.hex")
    actual_archive_sites = archive_sau_sites(archive_instruction)
    if actual_archive_sites != EXPECTED_ARCHIVE_SITES:
        raise RuntimeError(
            "archive SAU sites changed:\n"
            + json.dumps(actual_archive_sites, indent=2)
        )
    control_flow_observations = archive_control_flow_observations(
        archive_instruction
    )

    common_hash = sha256_bytes(COMMON_SOURCE.read_bytes())
    base_manifest = {
        "schema": "gem5-conv3-step1-fixture-v1",
        "provenance": "assembly-generated comparison fixture",
        "generator": "generate_fixtures.py",
        "toolchain": {
            "llvm_mc": str(Path(args.llvm_mc).resolve()),
            "llvm_mc_version": mc_version,
            "llvm_objcopy": str(Path(args.llvm_objcopy).resolve()),
            "llvm_objcopy_version": objcopy_version,
        },
        "archive": {
            "path": str(archive_path),
            "filename": archive_path.name,
            "sha256": archive_sha,
            "instruction_hex": {
                "member": "instruction.hex",
                "bytes": len(archive_instruction),
                "sha256": sha256_bytes(archive_instruction),
                "token_width_bits": 32,
                "token_count": WORD_COUNT,
                "byte_order": "little-endian word bytes",
            },
            "memory_hex": {
                "member": "memory.hex",
                "bytes": len(archive_memory),
                "sha256": sha256_bytes(archive_memory),
                "token_width_bits": 32,
                "token_count": WORD_COUNT,
                "byte_order": "little-endian word bytes",
            },
            "source_members": {
                name: {
                    "bytes": len(data),
                    "sha256": sha256_bytes(data),
                }
                for name, data in archive_sources.items()
            },
            "sau_static_sites": actual_archive_sites,
            "control_flow_observations": control_flow_observations,
            "sau_dynamic_count_from_archive_program_analysis": {
                "msetins1": 1,
                "msetins2": 1,
                "msetins3": 32,
                "msetins4": 32,
            },
        },
        "image": {
            "instruction_base": 0,
            "instruction_bytes": IMAGE_BYTES,
            "instruction_words": WORD_COUNT,
            "data_base": DATA_BASE,
            "real_data_bytes": DATA_CAPACITY,
            "format": "one 8-hex-digit 32-bit readmemh token per line",
            "word_to_cpu_bytes": "token value encoded little-endian",
        },
        "conv": {
            "N": 1,
            "C": 16,
            "H": 16,
            "W": 32,
            "OC": 16,
            "kernel": 3,
            "stride": 1,
            "padding": 1,
            "cutbit": 12,
            "input_base": 0x29130000,
            "weight_base": 0x29132000,
            "bias_base": 0x29132900,
            "output_base": 0x29132920,
            "padded_scratch_base": 0x29136000,
            "input_bytes": 8192,
            "weight_bytes": 2304,
            "bias_bytes": 32,
            "output_bytes": 8192,
            "padded_scratch_bytes": 13824,
            "input_initialization": "signed INT8 input[i] = i / 16, stored bytewise",
            "weight_bias_provenance": "bytes from comparison memory image at configured addresses",
        },
        "program_layout": {
            # These ranges are labels in conv3_fixture_common.S. Keeping them
            # in the manifest lets the final e2e report separate startup,
            # input-initialization, and software-padding retirement counts.
            "startup_region": [0x0000, 0x002C],
            "input_init_region": [0x002C, 0x0040],
            "padding_region": [0x200, 0x4000],
            "config_region": [0x4000, 0x5000],
            "shared_tail_pc": 0x5000,
            "termination": "ebreak at shared_tail_pc",
        },
        "sources": {
            "common": {
                "path": str(COMMON_SOURCE.relative_to(output_root)),
                "sha256": common_hash,
            },
        },
    }

    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="conv3-step1-") as temp_name:
        temp_root = Path(temp_name)
        for name, spec in FIXTURES.items():
            source = spec["source"]
            fixture_dir = output_root / name
            fixture_dir.mkdir(parents=True, exist_ok=True)
            object_file = temp_root / f"{name}.o"
            binary_file = temp_root / f"{name}.bin"
            instruction_file = fixture_dir / "instruction.hex"
            memory_file = fixture_dir / "memory.hex"

            mc_command = [
                args.llvm_mc,
                "--triple=riscv32-unknown-elf",
                "--filetype=obj",
                "-I", str(output_root),
                str(source),
                "-o", str(object_file),
            ]
            objcopy_command = [
                args.llvm_objcopy,
                "-O", "binary",
                "--only-section=.text",
                str(object_file),
                str(binary_file),
            ]
            run(mc_command)
            run(objcopy_command)
            instruction_hex = render_instruction_hex(binary_file.read_bytes())
            instruction_file.write_bytes(instruction_hex)
            memory_file.write_bytes(archive_memory)

            manifest = dict(base_manifest)
            manifest["fixture"] = {
                "name": name,
                "mode": spec["mode"],
                "purpose": spec["purpose"],
                "software_padding": spec["software_padding"],
                "sau_instruction_counts": spec["sau_counts"],
            }
            manifest["sources"] = dict(base_manifest["sources"])
            manifest["sources"]["entry"] = {
                "path": str(source.relative_to(output_root)),
                "sha256": sha256_bytes(source.read_bytes()),
            }
            manifest["commands"] = {
                "llvm_mc": [
                    args.llvm_mc,
                    "--triple=riscv32-unknown-elf",
                    "--filetype=obj",
                    "-I", "fixtures/conv3_step1",
                    str(source.relative_to(output_root)),
                    "-o", f"<temporary>/{name}.o",
                ],
                "llvm_objcopy": [
                    args.llvm_objcopy,
                    "-O", "binary",
                    "--only-section=.text",
                    f"<temporary>/{name}.o",
                    f"<temporary>/{name}.bin",
                ],
                "word_token_conversion": (
                    "read each 4-byte binary word as little-endian and emit "
                    "one lowercase 8-hex-digit token; zero-pad to 65536 words"
                ),
            }
            manifest["outputs"] = {
                "instruction_hex": {
                    "path": str(instruction_file.relative_to(output_root)),
                    "bytes": len(instruction_hex),
                    "sha256": sha256_bytes(instruction_hex),
                    "token_count": len(parse_word_tokens(
                        instruction_hex, f"{name} instruction.hex"
                    )),
                    "sau_instruction_counts_observed": generated_sau_counts(
                        instruction_hex
                    ),
                },
                "memory_hex": {
                    "path": str(memory_file.relative_to(output_root)),
                    "bytes": len(archive_memory),
                    "sha256": sha256_bytes(archive_memory),
                    "identical_to_archive_member": True,
                },
            }
            write_manifest(fixture_dir / "manifest.json", manifest)

    print(f"generated {len(FIXTURES)} fixtures under {output_root}")


if __name__ == "__main__":
    main()
