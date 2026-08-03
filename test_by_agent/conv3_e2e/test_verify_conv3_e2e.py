#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFY_PATH = Path(__file__).with_name("verify_conv3_e2e.py")
SPEC = importlib.util.spec_from_file_location("verify_conv3_e2e", VERIFY_PATH)
VERIFY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(VERIFY)


class Conv3E2EVerifierTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_dir = ROOT / "fixtures/conv3_step1/generated_four_ins_full_offload"
        cls.manifest = json.loads(
            (cls.fixture_dir / "manifest.json").read_text(encoding="utf-8")
        )
        cls.memory = VERIFY.read_memory_image(cls.fixture_dir / "memory.hex")

    def test_instruction_image_and_payloads(self):
        words = VERIFY.read_words(self.fixture_dir / "instruction.hex")
        report = VERIFY.verify_instruction_image(words, self.manifest, "conv3")
        self.assertEqual(report["sau_slots_in_image"], [1, 2, 3, 4])
        self.assertEqual(
            VERIFY.manifest_payloads(self.manifest["conv"]),
            [
                0x2913200029130000,
                0x2913292029132900,
                0x0410000100200010,
                0xC300000080001B11,
            ],
        )

    def test_retire_decoder_accepts_archive_register_operands(self):
        records = [
            {"retire": 1, "retire_instr": VERIFY.mset_word(1, 23, 10)},
            {"retire": 1, "retire_instr": VERIFY.mset_word(2, 11, 12)},
            {"retire": 1, "retire_instr": VERIFY.mset_word(3, 26, 21)},
            {"retire": 1, "retire_instr": VERIFY.mset_word(4, 23, 21)},
        ]
        self.assertEqual(
            VERIFY.retired_sau_counts(records),
            {"msetins1": 1, "msetins2": 1, "msetins3": 1, "msetins4": 1},
        )

    def test_normal_termination_accepts_generated_and_archive_pcs(self):
        generated_records = [{
            "edge": 9,
            "retire": 1,
            "retire_instr": 0x00100073,
            "retire_pc": self.manifest["program_layout"]["shared_tail_pc"],
        }]
        generated_report = VERIFY.verify_normal_termination(
            generated_records, self.manifest
        )
        self.assertEqual(generated_report["termination"], "ebreak")

        archive_manifest = json.loads(
            (ROOT / "test_by_agent/conv3_e2e/archive_baseline_manifest.json")
            .read_text(encoding="utf-8")
        )
        archive_records = [{
            "edge": 11,
            "retire": 1,
            "retire_instr": 0x00100073,
            "retire_pc": 0xc32,
        }]
        archive_report = VERIFY.verify_normal_termination(
            archive_records, archive_manifest
        )
        self.assertEqual(archive_report["termination_pc"], "0xc32")

    def test_reference_matches_frozen_zero_data_fixture(self):
        output, accumulators = VERIFY.reference_output(self.manifest, self.memory)
        self.assertEqual(len(output), self.manifest["conv"]["output_bytes"])
        self.assertEqual(hashlib.sha256(output).hexdigest(),
                         "9f1dcbc35c350d6027f98be0f5c8b43b42ca52b7604459c0c42be3aa88913d47")
        self.assertEqual(set(output), {0})
        self.assertEqual(set(accumulators), {0})

    def test_request_stream_checks_read_data_and_byte_writes(self):
        conv = self.manifest["conv"]
        expected_output, _ = VERIFY.reference_output(self.manifest, self.memory)
        read_addresses = VERIFY.expected_read_lines(conv)
        requests = []
        responses = []
        edge = 1
        expected_memory = VERIFY.memory_after_program_initialization(
            self.manifest, self.memory
        )
        data_base = self.manifest["image"]["data_base"]
        for address in read_addresses:
            requests.append({
                "edge": edge,
                "sau_sram_req": 1,
                "sau_sram_addr": address,
                "sau_sram_wstrb": 0,
            })
            offset = address - data_base
            responses.append({
                "edge": edge + 1,
                "sau_sram_resp": 1,
                "sau_sram_rdata": int.from_bytes(
                    expected_memory[offset:offset + 32], "little"
                ),
            })
            edge += 2

        for index, value in enumerate(expected_output):
            address = conv["output_base"] + index
            lane = address & 0x1F
            requests.append({
                "edge": edge,
                "sau_sram_req": 1,
                "sau_sram_addr": VERIFY.line_address(address),
                "sau_sram_wstrb": 1 << lane,
                "sau_sram_wdata": value << (lane * 8),
            })
            responses.append({
                "edge": edge + 1,
                "sau_sram_resp": 1,
                "sau_sram_rdata": 0,
            })
            edge += 2

        report = VERIFY.check_sau_request_stream(
            requests + responses, self.manifest, self.memory, expected_output
        )
        self.assertEqual(report["memory_requests"], 18704)
        self.assertEqual(report["read_requests"], 10512)
        self.assertEqual(report["write_requests"], 8192)
        self.assertEqual(report["output_sha256"],
                         "9f1dcbc35c350d6027f98be0f5c8b43b42ca52b7604459c0c42be3aa88913d47")


if __name__ == "__main__":
    unittest.main()
