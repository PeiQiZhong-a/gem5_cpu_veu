from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from compare_cycle_traces import compare, load_trace


HEADER_RTL = "# brs-cycle-trace-v1 source=rtl sampling=posedge-pre-nba\n"
HEADER_GEM5 = "# brs-cycle-trace-v1 source=gem5 sampling=posedge-pre-nba\n"
HEADER_GEM5_V2 = (
    "# brs-cycle-trace-v2 source=gem5 "
    "sampling=evaluate-before-clock platform=rtl-dut-kui-tb\n"
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


if __name__ == "__main__":
    unittest.main()
