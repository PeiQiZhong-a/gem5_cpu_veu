from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from compare_cycle_traces import compare, load_trace, select_comparison_window


HEADER_RTL = "# brs-cycle-trace-v1 source=rtl sampling=posedge-pre-nba\n"
HEADER_GEM5 = "# brs-cycle-trace-v1 source=gem5 sampling=posedge-pre-nba\n"
HEADER_GEM5_V2 = (
    "# brs-cycle-trace-v2 source=gem5 "
    "sampling=evaluate-before-clock platform=rtl-dut-kui-tb\n"
)
HEADER_RTL_V3 = (
    "# brs-cycle-trace-v3 source=rtl sampling=posedge-pre-nba "
    "platform=top_veu_regress_tb predictor_present=1 btb_enabled=0\n"
)
HEADER_GEM5_V3 = (
    "# brs-cycle-trace-v3 source=gem5 sampling=posedge-pre-nba "
    "platform=rtl-dut-kui-tb predictor_present=0 btb_enabled=0\n"
)


def cycle(edge: int, **fields: int) -> str:
    defaults = {
        "reset": 0, "cpu_cycle": edge, "ibus_req": 0, "ibus_resp": 0,
        "dbus_req": 0, "dbus_resp": 0, "grant": 0, "retire": 0,
        "stall_mask": 0, "redirect": 0, "done": 0, "done_value": 0,
    }
    defaults.update(fields)
    body = " ".join(f"{key}={value}" for key, value in defaults.items())
    return f"edge={edge} {body}\n"


def cycle_v3(edge: int, source: str, platform: str, **fields: int) -> str:
    defaults = {
        "reset": 0, "phase": "posedge-pre-nba", "source": source,
        "platform": platform, "cpu_cycle": edge,
        "ibus_req": 0, "ibus_addr": 0, "ibus_re": 0, "ibus_resp": 0,
        "ibus_r0": 0, "ibus_r1": 0, "ibus_r2": 0, "ibus_r3": 0,
        "dbus_req": 0, "dbus_addr": 0, "dbus_re": 0, "dbus_we": 0,
        "dbus_wstrb": 0, "dbus_wdata": 0, "dbus_resp": 0,
        "dbus_rdata": 0,
        "retire": 0, "retire_pc": 0, "retire_instr": 0, "wb_we": 0,
        "wb_fp": 0, "wb_rd": 0, "wb_data": 0, "stall_mask": 0,
        "redirect": 0, "redirect_target": 0, "grant": 0,
        "set_btb_off": 1, "btb_match": 0, "predict_failed": 0,
        "hc_req": 0, "hc_addr": 0, "hc_re": 0, "hc_we": 0,
        "hc_write_type": 0, "hc_wdata": 0, "hc_vestart": 0,
        "hc_valid": 0, "hc_rdata": 0,
        "done": 0, "done_value": 0, "error": 0, "error_value": 0,
    }
    defaults.update(fields)
    body = " ".join(f"{key}={value}" for key, value in defaults.items())
    return f"edge={edge} {body}\n"


class CompareCycleTracesTest(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def write(self, name: str, header: str, lines: list[str]) -> Path:
        path = Path(self.temp.name) / name
        path.write_text(header + "".join(lines), encoding="utf-8")
        return path

    def test_matching_done_retire_and_cycles(self):
        lines = [
            cycle(1, reset=1, cpu_cycle=0),
            cycle(2, retire=1, retire_pc=0, retire_instr=0x13,
                  wb_we=0),
            cycle(3, dbus_req=1, dbus_addr=0x4001E004, dbus_we=1,
                  dbus_re=0, dbus_wstrb=0xF, dbus_wdata=2,
                  done=1, done_value=2),
        ]
        rtl = load_trace(self.write("rtl.log", HEADER_RTL, lines))
        # New gem5 v2 output remains comparable with archived RTL v1 traces.
        gem5 = load_trace(self.write("gem5.log", HEADER_GEM5_V2, lines))
        self.assertTrue(compare(rtl, gem5, 2)["match"])

    def test_reports_first_retire_edge_and_cycle_mismatch(self):
        rtl_lines = [
            cycle(1, reset=1, cpu_cycle=0),
            cycle(2, retire=1, retire_pc=0, retire_instr=0x13, wb_we=0),
            cycle(3, done=1, done_value=2),
        ]
        gem5_lines = [
            cycle(1, reset=1, cpu_cycle=0),
            cycle(2),
            cycle(3, retire=1, retire_pc=0, retire_instr=0x13, wb_we=0,
                  done=1, done_value=2),
        ]
        rtl = load_trace(self.write("rtl.log", HEADER_RTL, rtl_lines))
        gem5 = load_trace(self.write("gem5.log", HEADER_GEM5, gem5_lines))
        report = compare(rtl, gem5, 1)
        self.assertFalse(report["match"])
        self.assertEqual(report["retire_mismatch_index"], 0)
        self.assertEqual(report["cycle_mismatch_edge"], 2)
        self.assertIn("retire", report["cycle_mismatch_fields"])

    def test_ignores_inactive_payload_pins(self):
        rtl_lines = [cycle(1, ibus_addr=1), cycle(2, done=1, done_value=2)]
        gem5_lines = [cycle(1, ibus_addr=99), cycle(2, done=1, done_value=2)]
        rtl = load_trace(self.write("rtl.log", HEADER_RTL, rtl_lines))
        gem5 = load_trace(self.write("gem5.log", HEADER_GEM5, gem5_lines))
        self.assertTrue(compare(rtl, gem5, 1)["match"])

    def test_v3_matching_complete_schema(self):
        rtl_lines = [
            cycle_v3(1, "rtl", "top_veu_regress_tb", hc_req=1,
                     hc_addr=0x101, hc_we=1, hc_write_type=3,
                     hc_vestart=1),
            cycle_v3(2, "rtl", "top_veu_regress_tb", hc_valid=1,
                     grant=1, done=1, done_value=2),
        ]
        gem5_lines = [
            cycle_v3(1, "gem5", "rtl-dut-kui-tb", hc_req=1,
                     hc_addr=0x101, hc_we=1, hc_write_type=3,
                     hc_vestart=1),
            cycle_v3(2, "gem5", "rtl-dut-kui-tb", hc_valid=1,
                     grant=1, done=1, done_value=2),
        ]
        rtl = load_trace(self.write("rtl-v3.log", HEADER_RTL_V3, rtl_lines))
        gem5 = load_trace(
            self.write("gem5-v3.log", HEADER_GEM5_V3, gem5_lines))
        self.assertTrue(compare(rtl, gem5, 1)["match"])

    def test_v3_rejects_missing_required_field(self):
        line = cycle_v3(1, "rtl", "top_veu_regress_tb")
        line = line.replace(" hc_vestart=0", "")
        path = self.write("missing.log", HEADER_RTL_V3, [line])
        with self.assertRaisesRegex(ValueError, "hc_vestart"):
            load_trace(path)

    def test_v3_rejects_unknown_required_value(self):
        line = cycle_v3(1, "rtl", "top_veu_regress_tb")
        line = line.replace(" dbus_req=0", " dbus_req=x")
        path = self.write("unknown.log", HEADER_RTL_V3, [line])
        with self.assertRaisesRegex(ValueError, "dbus_req"):
            load_trace(path)

    def test_v3_rejects_missing_edge(self):
        path = self.write(
            "gap.log", HEADER_RTL_V3,
            [cycle_v3(2, "rtl", "top_veu_regress_tb")])
        with self.assertRaisesRegex(ValueError, "not contiguous"):
            load_trace(path)

    def test_v3_reports_hc_payload_mismatch(self):
        rtl = load_trace(self.write(
            "rtl-hc.log", HEADER_RTL_V3,
            [cycle_v3(1, "rtl", "top_veu_regress_tb", hc_req=1,
                      hc_addr=0x101, hc_we=1),
             cycle_v3(2, "rtl", "top_veu_regress_tb", done=1,
                      done_value=2)]))
        gem5 = load_trace(self.write(
            "gem5-hc.log", HEADER_GEM5_V3,
            [cycle_v3(1, "gem5", "rtl-dut-kui-tb", hc_req=1,
                      hc_addr=0x102, hc_we=1),
             cycle_v3(2, "gem5", "rtl-dut-kui-tb", done=1,
                      done_value=2)]))
        report = compare(rtl, gem5, 1)
        self.assertFalse(report["match"])
        self.assertIn("hc_addr", report["cycle_mismatch_fields"])

    def test_application_window_removes_boot_prefix_and_stops_at_done(self):
        app_pc = 0x29110008
        app_lines = [
            cycle_v3(1, "rtl", "top_veu_regress_tb", retire=1,
                     retire_pc=app_pc, retire_instr=0x13),
            cycle_v3(2, "rtl", "top_veu_regress_tb", done=1,
                     done_value=2),
        ]
        rtl = load_trace(self.write(
            "rtl-window.log", HEADER_RTL_V3,
            [cycle_v3(1, "rtl", "top_veu_regress_tb"),
             cycle_v3(2, "rtl", "top_veu_regress_tb", retire=1,
                      retire_pc=0, retire_instr=0x13),
             cycle_v3(3, "rtl", "top_veu_regress_tb", retire=1,
                      retire_pc=app_pc, retire_instr=0x13),
             cycle_v3(4, "rtl", "top_veu_regress_tb", done=1,
                      done_value=2),
             cycle_v3(5, "rtl", "top_veu_regress_tb", retire=1,
                      retire_pc=0x100, retire_instr=0x13)]))
        gem5 = load_trace(self.write(
            "gem5-window.log", HEADER_GEM5_V3,
            [line.replace("source=rtl", "source=gem5").replace(
                "platform=top_veu_regress_tb", "platform=rtl-dut-kui-tb")
             for line in app_lines] +
            [cycle_v3(3, "gem5", "rtl-dut-kui-tb", retire=1,
                      retire_pc=0x100, retire_instr=0x13)]))

        rtl_window = select_comparison_window(rtl, app_pc, True)
        gem5_window = select_comparison_window(gem5, app_pc, True)
        report = compare(rtl_window, gem5_window, 1)
        self.assertTrue(report["match"])
        self.assertEqual(len(rtl_window.records), 2)
        self.assertEqual(len(gem5_window.records), 2)
        self.assertEqual(
            rtl_window.metadata["comparison_window_start_edge"], 3)
        self.assertEqual(
            gem5_window.metadata["comparison_window_start_edge"], 1)

    def test_application_window_requires_anchor_and_done(self):
        trace = load_trace(self.write(
            "no-anchor.log", HEADER_RTL_V3,
            [cycle_v3(1, "rtl", "top_veu_regress_tb")]))
        with self.assertRaisesRegex(ValueError, "anchor PC"):
            select_comparison_window(trace, 0x29110008, False)
        with self.assertRaisesRegex(ValueError, "no DONE"):
            select_comparison_window(trace, None, True)


if __name__ == "__main__":
    unittest.main()
