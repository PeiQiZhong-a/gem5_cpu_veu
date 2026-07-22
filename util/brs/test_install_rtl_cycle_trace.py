from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from install_rtl_cycle_trace import MARKER, install


class InstallRtlCycleTraceTest(unittest.TestCase):
    def test_install_update_and_idempotence_preserve_crlf(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            top = root / "aerith_tb_top.sv"
            monitor = root / "monitor.svinc"
            top.write_bytes(
                b"module aerith_tb_top;\r\n"
                b"    //========================================================================\r\n"
                b"    // Test Sequence\r\n"
                b"    //========================================================================\r\n"
                b"endmodule\r\n")
            monitor.write_text(
                "    //========================================================================\n"
                f"    {MARKER}\n"
                "    integer version = 1;\n\n",
                encoding="utf-8")

            self.assertTrue(install(top, monitor, False))
            installed = top.read_bytes()
            self.assertIn(b"integer version = 1", installed)
            self.assertNotIn(b"\n", installed.replace(b"\r\n", b""))
            self.assertFalse(install(top, monitor, False))

            monitor.write_text(
                "    //========================================================================\n"
                f"    {MARKER}\n"
                "    integer version = 2;\n\n",
                encoding="utf-8")
            self.assertTrue(install(top, monitor, False))
            updated = top.read_text(encoding="utf-8")
            self.assertIn("integer version = 2", updated)
            self.assertNotIn("integer version = 1", updated)
            self.assertEqual(updated.count(MARKER), 1)


if __name__ == "__main__":
    unittest.main()
