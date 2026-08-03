#!/usr/bin/env python3
"""Verify one SAU comparison-fixture gem5 run independently.

The verifier does not call gem5 or a C++ endpoint implementation. It derives
the four ABI payloads, reconstructs the runtime input initialization, computes
the reference INT8 convolution, and checks the HC/SRAM request and response
records in the gem5 cycle trace. The ``conv3`` mode remains only for validating
archived historical reports; new runs use ``stub`` or ``sau_n``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


STAT_PREFIX = "system.pipeline."
WORD_COUNT = 65536
SAU_FUNCT7_TO_SLOT = {0: 1, 3: 2, 6: 3, 9: 4}
SAU_SLOT_TO_FUNCT7 = {slot: funct7 for funct7, slot in SAU_FUNCT7_TO_SLOT.items()}
SAU_TARGET = 2
SAU_WRITE_TYPE_SET = 1


class VerificationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def parse_int(value: str) -> int:
    value = value.lower()
    if value.startswith("0x"):
        return int(value, 16)
    return int(value, 10)


def read_stats(path: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2 or not fields[0].startswith(STAT_PREFIX):
            continue
        try:
            result[fields[0]] = int(float(fields[1]))
        except ValueError:
            continue
    return result


def stat(stats: dict[str, int], name: str) -> int:
    key = STAT_PREFIX + name
    if key not in stats:
        fail(f"missing stat {key}")
    return stats[key]


def read_words(path: Path) -> list[int]:
    tokens = path.read_bytes().split()
    if len(tokens) != WORD_COUNT:
        fail(f"{path}: expected {WORD_COUNT} word tokens, got {len(tokens)}")
    words: list[int] = []
    for index, token in enumerate(tokens):
        try:
            text = token.decode("ascii")
            if len(text) != 8:
                raise ValueError
            value = int(text, 16)
        except (UnicodeDecodeError, ValueError) as error:
            fail(f"{path}: invalid token {index}: {token!r}")
            raise AssertionError from error
        if value > 0xFFFFFFFF:
            fail(f"{path}: token {index} exceeds 32 bits")
        words.append(value)
    return words


def read_memory_image(path: Path) -> bytes:
    tokens = path.read_bytes().split()
    if not tokens:
        fail(f"{path}: empty memory image")
    words: list[int] = []
    for index, token in enumerate(tokens):
        try:
            text = token.decode("ascii")
            if len(text) != 8:
                raise ValueError
            value = int(text, 16)
        except (UnicodeDecodeError, ValueError) as error:
            fail(f"{path}: invalid memory token {index}: {token!r}")
            raise AssertionError from error
        if value > 0xFFFFFFFF:
            fail(f"{path}: memory token {index} exceeds 32 bits")
        words.append(value)
    return b"".join(word.to_bytes(4, "little") for word in words)


def instruction_sau_slots(words: list[int]) -> list[int]:
    image = b"".join(word.to_bytes(4, "little") for word in words)
    slots: list[int] = []
    # The generated fixtures use .option norvc, but scanning 2-byte boundaries
    # also keeps this check valid for the archive's compressed-code layout.
    for offset in range(0, len(image) - 3, 2):
        word = int.from_bytes(image[offset:offset + 4], "little")
        if (word & 0x7F) != 0x6B or ((word >> 12) & 0x7) != 1:
            continue
        funct7 = (word >> 25) & 0x7F
        slot = SAU_FUNCT7_TO_SLOT.get(funct7)
        if slot is not None:
            slots.append(slot)
    return slots


def verify_instruction_image(
    words: list[int], manifest: dict[str, Any], sau_model: str
) -> dict[str, Any]:
    slots = instruction_sau_slots(words)
    mode = manifest.get("fixture", {}).get("mode", "")
    if mode.startswith("four_ins") and slots != [1, 2, 3, 4]:
        fail(f"four-instruction image SAU slots are {slots}, expected [1, 2, 3, 4]")
    if sau_model in ("conv3", "sau_n") and slots != [1, 2, 3, 4]:
        fail(
            f"{sau_model} requires exactly one ordered msetins1..4 image, "
            f"got {slots}"
        )
    return {
        "word_count": len(words),
        "sau_slots_in_image": slots,
    }


def verify_image_hash(path: Path, manifest: dict[str, Any], image_name: str) -> str:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    expected = (
        manifest.get("outputs", {})
        .get(image_name, {})
        .get("sha256")
    )
    if expected is not None and digest != expected:
        fail(f"{path}: SHA-256 {digest} does not match manifest {expected}")
    return digest


def read_trace(path: Path) -> list[dict[str, int]]:
    records: list[dict[str, int]] = []
    saw_header = False
    previous_edge = 0
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            if "brs-cycle-trace-v2" in line:
                saw_header = True
            continue
        fields: dict[str, int] = {}
        for token in line.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                fields[key] = parse_int(value)
            except ValueError:
                # v2 traces intentionally carry descriptive fields such as
                # phase/platform alongside numeric signals.
                continue
        if "edge" not in fields:
            continue
        if fields["edge"] <= previous_edge:
            fail(f"{path}:{line_number}: edge is not strictly increasing")
        if "reset" not in fields or "cpu_cycle" not in fields:
            fail(f"{path}:{line_number}: missing reset/cpu_cycle")
        previous_edge = fields["edge"]
        records.append(fields)
    if not saw_header:
        fail(f"{path}: missing brs-cycle-trace-v2 header")
    if not records:
        fail(f"{path}: no cycle records")
    return records


def active_records(records: list[dict[str, int]]) -> list[dict[str, int]]:
    return [record for record in records if record.get("reset") == 0]


def mset_word(slot: int, rs1: int = 5, rs2: int = 6) -> int:
    return (
        (SAU_SLOT_TO_FUNCT7[slot] << 25)
        | (rs2 << 20)
        | (rs1 << 15)
        | (1 << 12)
        | 0x6B
    )


def sau_slot_from_word(word: int) -> int | None:
    if (word & 0x7F) != 0x6B or ((word >> 12) & 0x7) != 1:
        return None
    return SAU_FUNCT7_TO_SLOT.get((word >> 25) & 0x7F)


def manifest_payloads(conv: dict[str, Any]) -> list[int]:
    stride_minus_one = int(conv["stride"]) - 1
    return [
        int(conv["input_base"]) | (int(conv["weight_base"]) << 32),
        int(conv["bias_base"]) | (int(conv["output_base"]) << 32),
        int(conv["H"])
        | (int(conv["W"]) << 16)
        | (int(conv["N"]) << 32)
        | (int(conv["C"]) << 48)
        | (int(conv["OC"]) << 54),
        1
        | (int(conv["padding"]) << 4)
        | (stride_minus_one << 5)
        | (int(conv["cutbit"]) << 6)
        | (int(conv["kernel"]) << 11)
        | (1 << 31)
        | (0xC3 << 56),
    ]


def signed_int8(value: int) -> int:
    return value if value < 128 else value - 256


def signed_int16_le(data: bytes, offset: int) -> int:
    raw = data[offset] | (data[offset + 1] << 8)
    return raw if raw < 0x8000 else raw - 0x10000


def saturate24(value: int) -> int:
    return max(-(1 << 23), min((1 << 23) - 1, value))


def reference_output(manifest: dict[str, Any], memory: bytes) -> tuple[bytes, list[int]]:
    image = manifest["image"]
    conv = manifest["conv"]
    data_base = int(image["data_base"])

    def byte_at(address: int) -> int:
        offset = address - data_base
        if offset < 0 or offset >= len(memory):
            fail(f"reference address 0x{address:x} is outside data image")
        return memory[offset]

    def weight_at(channel: int, kernel_h: int, kernel_w: int, output_channel: int) -> int:
        index = (((channel * int(conv["kernel"]) + kernel_h)
                  * int(conv["kernel"]) + kernel_w)
                 * int(conv["OC"]) + output_channel)
        return signed_int8(byte_at(int(conv["weight_base"]) + index))

    input_h = int(conv["H"])
    input_w = int(conv["W"])
    input_c = int(conv["C"])
    output_c = int(conv["OC"])
    kernel = int(conv["kernel"])
    padding = int(conv["padding"])
    stride = int(conv["stride"])
    output_h = (input_h + 2 * padding - kernel) // stride + 1
    output_w = (input_w + 2 * padding - kernel) // stride + 1
    input_bytes = int(conv["input_bytes"])
    expected_input_bytes = int(conv["N"]) * input_c * input_h * input_w
    if input_bytes != expected_input_bytes:
        fail("manifest input_bytes does not match NCHW shape")

    output = bytearray()
    accumulators: list[int] = []
    for n in range(int(conv["N"])):
        for output_channel in range(output_c):
            for output_h_index in range(output_h):
                for output_w_index in range(output_w):
                    accumulator = 0
                    for channel in range(input_c):
                        for kernel_h in range(kernel):
                            for kernel_w in range(kernel):
                                input_h_index = (
                                    output_h_index * stride + kernel_h - padding
                                )
                                input_w_index = (
                                    output_w_index * stride + kernel_w - padding
                                )
                                if (
                                    input_h_index < 0
                                    or input_h_index >= input_h
                                    or input_w_index < 0
                                    or input_w_index >= input_w
                                ):
                                    activation = 0
                                else:
                                    input_index = (
                                        (((n * input_c + channel) * input_h
                                          + input_h_index) * input_w)
                                        + input_w_index
                                    )
                                    # The fixture program stores i / 16 with
                                    # an RV32 byte store, so values above 127
                                    # are interpreted as signed INT8 here.
                                    activation = signed_int8(
                                        (input_index // 16) & 0xFF
                                    )
                                accumulator = saturate24(
                                    accumulator
                                    + activation
                                    * weight_at(
                                        channel,
                                        kernel_h,
                                        kernel_w,
                                        output_channel,
                                    )
                                )
                    bias = signed_int16_le(
                        memory,
                        int(conv["bias_base"]) - data_base
                        + output_channel * 2,
                    )
                    accumulator = saturate24(accumulator + bias)
                    quantized = accumulator >> int(conv["cutbit"])
                    quantized = max(-128, min(127, quantized))
                    output.append(quantized & 0xFF)
                    accumulators.append(accumulator)
    if len(output) != int(conv["output_bytes"]):
        fail("reference output length does not match manifest")
    return bytes(output), accumulators


def memory_after_program_initialization(
    manifest: dict[str, Any], memory: bytes
) -> bytes:
    conv = manifest["conv"]
    data_base = int(manifest["image"]["data_base"])
    initialized = bytearray(memory)

    initialization = manifest.get("initialization", {})
    modes = initialization.get("modes", {})
    input_mode = modes.get(
        "input", conv.get("input_initialization_mode", "cpu")
    )
    weight_mode = modes.get(
        "weight", conv.get("weight_initialization_mode", "memory")
    )
    bias_mode = modes.get(
        "bias", conv.get("bias_initialization_mode", "memory")
    )

    input_offset = int(conv["input_base"]) - data_base
    input_bytes = int(conv["input_bytes"])
    if input_offset < 0 or input_offset + input_bytes > len(memory):
        fail("input initialization range is outside the data image")
    source_regions = initialization.get("source_regions", {})
    if input_mode == "cpu":
        input_source = source_regions.get("input", {}).get("source_base")
        if input_source is None:
            for index in range(input_bytes):
                initialized[input_offset + index] = (index // 16) & 0xFF
        else:
            source_offset = int(input_source) - data_base
            if (
                source_offset < 0
                or source_offset + input_bytes > len(memory)
            ):
                fail("input initialization source is outside the data image")
            initialized[input_offset:input_offset + input_bytes] = memory[
                source_offset:source_offset + input_bytes
            ]
    elif input_mode != "memory":
        fail(f"unsupported input initialization mode: {input_mode}")

    for name, mode, base_key, bytes_key in (
        ("weight", weight_mode, "weight_base", "weight_bytes"),
        ("bias", bias_mode, "bias_base", "bias_bytes"),
    ):
        if mode == "memory":
            continue
        if mode != "cpu":
            fail(f"unsupported {name} initialization mode: {mode}")
        source = source_regions.get(name, {}).get("source_base")
        if source is None:
            fail(f"manifest lacks the {name} CPU initialization source")
        source_offset = int(source) - data_base
        target_offset = int(conv[base_key]) - data_base
        size = int(conv[bytes_key])
        if (
            source_offset < 0
            or source_offset + size > len(memory)
            or target_offset < 0
            or target_offset + size > len(memory)
        ):
            fail(f"{name} initialization range is outside the data image")
        initialized[target_offset:target_offset + size] = memory[
            source_offset:source_offset + size
        ]
    return bytes(initialized)


def line_address(address: int) -> int:
    return address & ~0x1F


def expected_read_lines(conv: dict[str, Any]) -> list[int]:
    reads = [
        line_address(int(conv["input_base"]) + index)
        for index in range(int(conv["input_bytes"]))
    ]
    reads.extend(
        line_address(int(conv["weight_base"]) + index)
        for index in range(int(conv["weight_bytes"]))
    )
    reads.extend(
        line_address(int(conv["bias_base"]) + output_channel * 2)
        for output_channel in range(int(conv["OC"]))
    )
    return reads


def trace_payloads(records: list[dict[str, int]]) -> dict[int, int]:
    payloads: dict[int, int] = {}
    for record in records:
        if (
            record.get("hc_req") != 1
            or record.get("hc_target") != SAU_TARGET
            or record.get("hc_we") != 1
        ):
            continue
        address = record.get("hc_addr")
        payload = record.get("hc_wdata")
        if address is None or payload is None:
            fail("trace lacks hc_addr/hc_wdata fields required by Conv3/sau_n e2e")
        if address not in (0x200, 0x202, 0x204, 0x206):
            continue
        previous = payloads.setdefault(address, payload)
        if previous != payload:
            fail(f"HC payload changed while request 0x{address:x} was held")
    return payloads


def read_sau_n_output(path: Path) -> tuple[int, bytes, int]:
    event: dict[str, str] | None = None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        fields = dict(
            token.split("=", 1)
            for token in raw_line.split()
            if "=" in token
        )
        if fields.get("event") != "sau_n_output":
            continue
        if event is not None:
            fail(f"{path}: expected exactly one sau_n_output event")
        event = fields
    if event is None:
        fail(f"{path}: missing sau_n_output event")
    try:
        output_base = parse_int(event["output_base"])
        output_bytes = int(event["output_bytes"], 0)
        operation = int(event["operation"], 0)
        data = bytes.fromhex(event["data_hex"])
    except (KeyError, ValueError) as error:
        fail(f"{path}: malformed sau_n_output event: {error}")
    if operation != 1:
        fail(f"{path}: sau_n_output operation is {operation}, expected 1")
    if len(data) != output_bytes:
        fail(
            f"{path}: sau_n_output has {len(data)} bytes, "
            f"expected {output_bytes}"
        )
    return output_base, data, output_bytes


def retired_sau_counts(records: list[dict[str, int]]) -> dict[str, int]:
    result = {f"msetins{slot}": 0 for slot in range(1, 5)}
    for record in records:
        if record.get("retire") != 1:
            continue
        slot = sau_slot_from_word(record.get("retire_instr", 0))
        if slot is not None:
            result[f"msetins{slot}"] += 1
    return result


def retired_sau_sequence(records: list[dict[str, int]]) -> list[int]:
    return [
        slot
        for record in records
        if record.get("retire") == 1
        for slot in [sau_slot_from_word(record.get("retire_instr", 0))]
        if slot is not None
    ]


def verify_normal_termination(
    records: list[dict[str, int]], manifest: dict[str, Any]
) -> dict[str, Any]:
    layout = manifest.get("program_layout", {})
    expected_pcs: list[int] = []
    if "shared_tail_pc" in layout:
        expected_pcs.append(int(layout["shared_tail_pc"]))
    expected_pcs.extend(
        int(pc)
        for pc in manifest.get("archive", {})
        .get("control_flow_observations", {})
        .get("static_compressed_ebreak_pcs", [])
    )
    if not expected_pcs:
        fail("manifest does not declare an ebreak termination PC")
    terminations = [
        record for record in records
        if record.get("retire") == 1
        and record.get("retire_instr") == 0x00100073
        and record.get("retire_pc") in expected_pcs
    ]
    if len(terminations) != 1:
        fail(
            "expected exactly one normal ebreak termination at "
            f"{[hex(pc) for pc in expected_pcs]}, found {len(terminations)}"
        )
    return {
        "termination": "ebreak",
        "termination_pc": hex(terminations[0]["retire_pc"]),
        "termination_edge": terminations[0]["edge"],
    }


def check_sau_request_stream(
    records: list[dict[str, int]],
    manifest: dict[str, Any],
    memory: bytes,
    expected_output: bytes,
) -> dict[str, Any]:
    conv = manifest["conv"]
    requests = [record for record in records if record.get("sau_sram_req") == 1]
    responses = [record for record in records if record.get("sau_sram_resp") == 1]
    if len(requests) != len(responses):
        fail(
            f"SAU request/response count is {len(requests)}/{len(responses)}"
        )
    if any(record.get("sau_sram_drop") == 1 for record in records):
        fail("SAU SRAM request was dropped by the crossbar")

    read_requests = [record for record in requests if record.get("sau_sram_wstrb", 0) == 0]
    write_requests = [record for record in requests if record.get("sau_sram_wstrb", 0) != 0]
    expected_reads = expected_read_lines(conv)
    actual_reads = [record.get("sau_sram_addr") for record in read_requests]
    if actual_reads != expected_reads:
        fail("SAU read line sequence differs from the NCHW/weight/bias reference")

    expected_memory = memory_after_program_initialization(manifest, memory)
    data_base = int(manifest["image"]["data_base"])
    for request, response in zip(requests, responses):
        if request.get("sau_sram_wstrb", 0) != 0:
            continue
        address = request.get("sau_sram_addr")
        response_data = response.get("sau_sram_rdata")
        if address is None or response_data is None:
            fail("trace lacks SRAM response data required for input/weight/bias checking")
        offset = address - data_base
        if offset < 0 or offset + 32 > len(expected_memory):
            fail(f"SAU read response line is outside the data image at 0x{address:x}")
        expected_data = int.from_bytes(
            expected_memory[offset:offset + 32], "little"
        )
        if response_data != expected_data:
            fail(
                f"SAU read data mismatch at 0x{address:x}: "
                f"actual=0x{response_data:x} expected=0x{expected_data:x}"
            )

    output_base = int(conv["output_base"])
    output_bytes = int(conv["output_bytes"])
    written: dict[int, int] = {}
    expected_writes: list[tuple[int, int, int]] = []
    for index, value in enumerate(expected_output):
        address = output_base + index
        lane = address & 0x1F
        expected_writes.append((line_address(address), 1 << lane, value << (lane * 8)))

    actual_writes: list[tuple[int, int, int]] = []
    for record in write_requests:
        address = record.get("sau_sram_addr")
        strobe = record.get("sau_sram_wstrb")
        data = record.get("sau_sram_wdata")
        if address is None or strobe is None or data is None:
            fail("trace lacks complete SAU SRAM write fields")
        if strobe == 0 or strobe & (strobe - 1):
            fail("Conv3 D write does not contain exactly one valid byte")
        lane = strobe.bit_length() - 1
        value = (data >> (lane * 8)) & 0xFF
        byte_address = address + lane
        if not output_base <= byte_address < output_base + output_bytes:
            fail(f"SAU wrote outside D tensor at 0x{byte_address:x}")
        written[byte_address] = value
        actual_writes.append((address, strobe, value << (lane * 8)))

    if actual_writes != expected_writes:
        fail("SAU D write sequence/data differs from the reference output")
    if len(written) != output_bytes:
        fail("SAU D writes do not cover every output byte exactly once")

    final_request_edge = requests[-1]["edge"]
    final_response_edge = responses[-1]["edge"]
    if final_response_edge <= final_request_edge:
        fail("final D write response did not follow the final D request")

    return {
        "memory_requests": len(requests),
        "memory_responses": len(responses),
        "read_requests": len(read_requests),
        "write_requests": len(write_requests),
        "final_write_request_edge": final_request_edge,
        "final_write_response_edge": final_response_edge,
        "output_sha256": hashlib.sha256(bytes(written[address]
                                               for address in sorted(written))).hexdigest(),
    }


def verify_stub(
    records: list[dict[str, int]],
    stats: dict[str, int],
    manifest: dict[str, Any],
    instruction_report: dict[str, Any],
) -> dict[str, Any]:
    expected_counts = manifest["fixture"]["sau_instruction_counts"]
    actual_counts = retired_sau_counts(records)
    if actual_counts != expected_counts:
        fail(f"retired SAU counts {actual_counts} != {expected_counts}")
    total = sum(expected_counts.values())
    if stat(stats, "sau_issue_count") != total:
        fail("sau_issue_count does not match retired fixture instructions")
    if stat(stats, "sau_retire_count") != total:
        fail("sau_retire_count does not match fixture instructions")
    if stat(stats, "sau_memory_requests") != 0:
        fail("StubSau unexpectedly issued SRAM requests")
    if stat(stats, "sau_operation_start_count") != 0:
        fail("StubSau unexpectedly started a Conv3 operation")
    return {
        "sau_instruction_counts": actual_counts,
        "sau_issue_count": stat(stats, "sau_issue_count"),
        "sau_retire_count": stat(stats, "sau_retire_count"),
        "memory_requests": 0,
        "instruction_image": instruction_report,
    }


def verify_conv3(
    records: list[dict[str, int]],
    stats: dict[str, int],
    manifest: dict[str, Any],
    memory: bytes,
    instruction_report: dict[str, Any],
) -> dict[str, Any]:
    expected_counts = {
        "msetins1": 1,
        "msetins2": 1,
        "msetins3": 1,
        "msetins4": 1,
    }
    actual_counts = retired_sau_counts(records)
    if actual_counts != expected_counts:
        fail(f"retired Conv3 SAU counts {actual_counts} != {expected_counts}")
    retired_slots = retired_sau_sequence(records)
    if retired_slots != [1, 2, 3, 4]:
        fail(f"retired Conv3 SAU sequence is {retired_slots}, expected [1, 2, 3, 4]")

    payloads = manifest_payloads(manifest["conv"])
    traced_payloads = trace_payloads(records)
    expected_addresses = [0x200, 0x202, 0x204, 0x206]
    actual_payloads = [traced_payloads.get(address) for address in expected_addresses]
    if actual_payloads != payloads:
        fail(
            "HC payload mismatch: "
            f"actual={[None if value is None else hex(value) for value in actual_payloads]} "
            f"expected={[hex(value) for value in payloads]}"
        )
    for address in expected_addresses:
        matching = [
            record
            for record in records
            if record.get("hc_req") == 1
            and record.get("hc_target") == SAU_TARGET
            and record.get("hc_addr") == address
            and record.get("hc_we") == 1
        ]
        if any(record.get("hc_write_type") != SAU_WRITE_TYPE_SET for record in matching):
            fail(f"HC address 0x{address:x} was not issued as a Set write")

    responses = [
        record
        for record in records
        if record.get("hc_valid") == 1 and record.get("hc_target") == SAU_TARGET
        and record.get("hc_we") == 1
    ]
    response_addresses = [record.get("hc_addr") for record in responses]
    if response_addresses != expected_addresses:
        fail(f"SAU HC response sequence {response_addresses} != {expected_addresses}")

    expected_memory = memory_after_program_initialization(manifest, memory)
    expected_output, _ = reference_output(manifest, expected_memory)
    memory_report = check_sau_request_stream(
        records, manifest, memory, expected_output
    )
    final_response_edge = memory_report["final_write_response_edge"]
    mset4_response = responses[-1]
    if mset4_response["edge"] <= final_response_edge:
        fail("msetins4 completed before the final D write response")
    done_records = [record for record in records if record.get("sau_xbar_done") == 1]
    if len(done_records) != 1:
        fail(f"expected one crossbarDone pulse, got {len(done_records)}")
    done = done_records[0]
    if done["edge"] != mset4_response["edge"]:
        fail("crossbarDone and msetins4 completion are not aligned")
    if done.get("sau_sram_req") == 1:
        fail("crossbarDone tick also exposed an SRAM request")

    startup = manifest.get("startup", {})
    polls = startup.get("mgetins4_lsb_polls", {})
    expected_sau_instruction_count = 4 + int(
        polls.get("before_submit", 0)
    ) + int(polls.get("after_submit", 0))
    expected_stats = {
        "sau_issue_count": expected_sau_instruction_count,
        "sau_retire_count": expected_sau_instruction_count,
        "sau_operation_start_count": 1,
        "sau_operation_complete_count": 1,
        "sau_memory_requests": int(manifest["conv"]["input_bytes"])
        + int(manifest["conv"]["weight_bytes"])
        + int(manifest["conv"]["OC"])
        + int(manifest["conv"]["output_bytes"]),
    }
    for name, expected in expected_stats.items():
        if stat(stats, name) != expected:
            fail(f"{name} is {stat(stats, name)}, expected {expected}")
    if stat(stats, "sau_compute_wait_cycles") <= 0:
        fail("Conv3 compute wait statistic is empty")
    if stat(stats, "sau_writeback_wait_cycles") <= 0:
        fail("Conv3 writeback wait statistic is empty")
    if stat(stats, "sau_roi_end_cycle") <= stat(stats, "sau_roi_start_cycle"):
        fail("Conv3 ROI cycle range is empty")

    return {
        "sau_instruction_counts": actual_counts,
        "payloads": [hex(value) for value in payloads],
        "expected_output_sha256": hashlib.sha256(expected_output).hexdigest(),
        "compute_wait_cycles": stat(stats, "sau_compute_wait_cycles"),
        "writeback_wait_cycles": stat(stats, "sau_writeback_wait_cycles"),
        "roi_start_cycle": stat(stats, "sau_roi_start_cycle"),
        "roi_end_cycle": stat(stats, "sau_roi_end_cycle"),
        "instruction_image": instruction_report,
        **memory_report,
    }


def verify_sau_n(
    records: list[dict[str, int]],
    stats: dict[str, int],
    manifest: dict[str, Any],
    memory: bytes,
    instruction_report: dict[str, Any],
    trace_path: Path,
) -> dict[str, Any]:
    expected_counts = {
        "msetins1": 1,
        "msetins2": 1,
        "msetins3": 1,
        "msetins4": 1,
    }
    actual_counts = retired_sau_counts(records)
    if actual_counts != expected_counts:
        fail(f"retired sau_n SAU counts {actual_counts} != {expected_counts}")
    retired_slots = retired_sau_sequence(records)
    if retired_slots != [1, 2, 3, 4]:
        fail(
            f"retired sau_n SAU sequence is {retired_slots}, "
            "expected [1, 2, 3, 4]"
        )

    payloads = manifest_payloads(manifest["conv"])
    traced_payloads = trace_payloads(records)
    expected_addresses = [0x200, 0x202, 0x204, 0x206]
    actual_payloads = [traced_payloads.get(address) for address in expected_addresses]
    if actual_payloads != payloads:
        fail(
            "HC payload mismatch: "
            f"actual={[None if value is None else hex(value) for value in actual_payloads]} "
            f"expected={[hex(value) for value in payloads]}"
        )
    for address in expected_addresses:
        matching = [
            record
            for record in records
            if record.get("hc_req") == 1
            and record.get("hc_target") == SAU_TARGET
            and record.get("hc_addr") == address
            and record.get("hc_we") == 1
        ]
        if any(record.get("hc_write_type") != SAU_WRITE_TYPE_SET for record in matching):
            fail(f"HC address 0x{address:x} was not issued as a Set write")

    responses = [
        record
        for record in records
        if record.get("hc_valid") == 1 and record.get("hc_target") == SAU_TARGET
        and record.get("hc_we") == 1
    ]
    response_addresses = [record.get("hc_addr") for record in responses]
    if response_addresses != expected_addresses:
        fail(f"sau_n HC response sequence {response_addresses} != {expected_addresses}")

    external_requests = [
        record for record in records if record.get("sau_sram_req") == 1
    ]
    external_responses = [
        record for record in records if record.get("sau_sram_resp") == 1
    ]
    if external_requests or external_responses:
        fail(
            "sau_n LocalScratchpadBacking unexpectedly exposed external SRAM "
            f"traffic: requests={len(external_requests)} "
            f"responses={len(external_responses)}"
        )
    if any(record.get("sau_sram_drop") == 1 for record in records):
        fail("sau_n LocalScratchpadBacking exposed a dropped SRAM request")

    starts = [record for record in records if record.get("sau_xbar_start") == 1]
    dones = [record for record in records if record.get("sau_xbar_done") == 1]
    if len(starts) != 1 or len(dones) != 1:
        fail(
            f"expected one sau_n crossbar start/done pulse, "
            f"got {len(starts)}/{len(dones)}"
        )
    if starts[0]["edge"] >= dones[0]["edge"]:
        fail("sau_n crossbarDone did not follow crossbarStart")
    if responses[-1]["edge"] <= dones[0]["edge"]:
        fail("sau_n msetins4 completion was visible before crossbarDone")

    conv = manifest["conv"]
    expected_output_bytes = int(conv["output_bytes"])
    expected_memory = memory_after_program_initialization(manifest, memory)
    expected_output, _ = reference_output(manifest, expected_memory)
    output_base, actual_output, output_bytes = read_sau_n_output(trace_path)
    if output_base != int(conv["output_base"]):
        fail(
            f"sau_n output trace base 0x{output_base:x} != "
            f"manifest base 0x{int(conv['output_base']):x}"
        )
    if output_bytes != expected_output_bytes:
        fail(
            f"sau_n output trace size {output_bytes} != "
            f"manifest size {expected_output_bytes}"
        )
    if actual_output != expected_output:
        mismatch = next(
            index
            for index, (actual, expected) in enumerate(
                zip(actual_output, expected_output)
            )
            if actual != expected
        )
        fail(
            f"sau_n D output mismatch at byte {mismatch}: "
            f"actual=0x{actual_output[mismatch]:02x} "
            f"expected=0x{expected_output[mismatch]:02x}"
        )

    startup = manifest.get("startup", {})
    polls = startup.get("mgetins4_lsb_polls", {})
    expected_sau_instruction_count = 4 + int(
        polls.get("before_submit", 0)
    ) + int(polls.get("after_submit", 0))
    expected_stats = {
        "sau_issue_count": expected_sau_instruction_count,
        "sau_retire_count": expected_sau_instruction_count,
        "sau_operation_start_count": 1,
        "sau_operation_complete_count": 1,
        "sau_n_operation_start_count": 1,
        "sau_n_operation_complete_count": 1,
        "sau_n_spad_write_requests_d": expected_output_bytes,
        "sau_n_spad_write_grants_d": expected_output_bytes,
        "sau_n_output_elements": expected_output_bytes,
    }
    for name, expected in expected_stats.items():
        if stat(stats, name) != expected:
            fail(f"{name} is {stat(stats, name)}, expected {expected}")

    for bank in ("a", "b", "c"):
        requests = stat(stats, f"sau_n_spad_read_requests_{bank}")
        grants = stat(stats, f"sau_n_spad_read_grants_{bank}")
        responses_count = stat(stats, f"sau_n_spad_read_responses_{bank}")
        if requests <= 0 or grants <= 0 or responses_count <= 0:
            fail(f"sau_n {bank} scratchpad traffic is empty")
        if grants > requests or responses_count != grants:
            fail(
                f"sau_n {bank} scratchpad traffic is inconsistent: "
                f"requests/grants/responses={requests}/{grants}/{responses_count}"
            )

    if stat(stats, "sau_n_model_ticks") <= 0:
        fail("sau_n model tick statistic is empty")
    if stat(stats, "sau_n_d_pending_peak") <= 0:
        fail("sau_n D pending queue never became active")
    if stat(stats, "sau_n_roi_end_cycle") <= stat(stats, "sau_n_roi_start_cycle"):
        fail("sau_n ROI cycle range is empty")

    return {
        "sau_instruction_counts": actual_counts,
        "payloads": [hex(value) for value in payloads],
        "expected_output_sha256": hashlib.sha256(expected_output).hexdigest(),
        "actual_output_sha256": hashlib.sha256(actual_output).hexdigest(),
        "model_ticks": stat(stats, "sau_n_model_ticks"),
        "roi_start_cycle": stat(stats, "sau_n_roi_start_cycle"),
        "roi_end_cycle": stat(stats, "sau_n_roi_end_cycle"),
        "external_sram_requests": 0,
        "external_sram_responses": 0,
        "output_bytes": output_bytes,
        "instruction_image": instruction_report,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--instruction-hex", type=Path, required=True)
    parser.add_argument("--stats", type=Path, required=True)
    parser.add_argument("--cycle-trace", type=Path, required=True)
    parser.add_argument(
        "--sau-model", choices=("stub", "conv3", "sau_n"), required=True
    )
    parser.add_argument("--json-report", type=Path)
    args = parser.parse_args()

    fixture_dir = args.fixture_dir.resolve()
    manifest = json.loads((fixture_dir / "manifest.json").read_text(encoding="utf-8"))
    instruction_sha256 = verify_image_hash(
        args.instruction_hex, manifest, "instruction_hex"
    )
    instruction_report = verify_instruction_image(
        read_words(args.instruction_hex), manifest, args.sau_model
    )
    records = active_records(read_trace(args.cycle_trace))
    stats = read_stats(args.stats)
    if not records:
        fail("cycle trace has no active records")
    if stat(stats, "cycle_count") <= 0:
        fail("cycle_count is not positive")
    termination_report = verify_normal_termination(records, manifest)

    memory_sha256 = None
    if args.sau_model == "stub":
        details = verify_stub(records, stats, manifest, instruction_report)
    else:
        memory_path = fixture_dir / "memory.hex"
        memory_sha256 = verify_image_hash(memory_path, manifest, "memory_hex")
        memory = read_memory_image(memory_path)
        if args.sau_model == "conv3":
            details = verify_conv3(
                records, stats, manifest, memory, instruction_report
            )
        else:
            details = verify_sau_n(
                records,
                stats,
                manifest,
                memory,
                instruction_report,
                args.cycle_trace,
            )

    report = {
        "status": "PASS",
        "fixture": manifest["fixture"]["name"],
        "sau_model": args.sau_model,
        "instruction_sha256": instruction_sha256,
        "memory_sha256": memory_sha256,
        "cycle_count": stat(stats, "cycle_count"),
        "retired_inst_count": stat(stats, "retired_inst_count"),
        "stall_count": stat(stats, "stall_count"),
        "sau_csr_handshake_cycles": stat(stats, "sau_csr_handshake_cycles"),
        "roi_start_retired_inst": stat(stats, "sau_roi_start_retired_inst"),
        "roi_end_retired_inst": stat(stats, "sau_roi_end_retired_inst"),
        "memory_profile": manifest.get("image", {}),
        "initialization": manifest.get("initialization", {}),
        "startup": manifest.get("startup", {}),
        "termination": termination_report,
        "details": details,
    }
    if args.json_report:
        args.json_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, VerificationError, ValueError) as error:
        print(f"SAU e2e verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
