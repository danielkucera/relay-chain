import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "relayctl.py"


class RelayCliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, timeout=5,
        )

    def assert_frame(self, expected, *args):
        master, slave = pty.openpty()
        try:
            result = self.run_cli("--port", os.ttyname(slave), *args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(select.select([master], [], [], 1)[0])
            self.assertEqual(os.read(master, 100), bytes.fromhex(expected))
            self.assertIn("Sent:", result.stdout)
        finally:
            os.close(slave)
            os.close(master)

    def test_commands(self):
        for address in ("ff", "FF", "0xff", "255", "broadcast"):
            with self.subTest(address=address):
                self.assert_frame("ff 02 00 00 00 00 fd", address, "init")
        self.assert_frame("00 02 00 00 00 00 02", "0", "init")
        self.assert_frame("00 ff 00 00 00 00 ff", "0", "on")
        self.assert_frame("ff ff 00 00 00 00 00", "ff", "on")
        self.assert_frame("02 00 00 00 00 00 02", "02", "off")
        self.assert_frame("0a 00 00 00 00 00 0a", "10", "off")
        self.assert_frame("fe 00 00 00 00 00 fe", "0xfe", "off")
        self.assert_frame("ff 01 12 34 56 78 f6", "broadcast", "on-for", "305419896")
        self.assert_frame("01 01 ff ff ff ff 00", "1", "on-for", "4294967295")
        self.assert_frame("00 01 00 00 00 00 01", "0", "on-for", "0")

    def test_invalid_inputs(self):
        for args in (
            ("on",),
            ("init",),
            ("off",),
            ("on-for", "100"),
            ("256", "off"),
            ("-1", "off"),
            ("broadcast", "on-for", "-1"),
            ("0", "on-for", "4294967296"),
            ("0", "on-for", "x"),
        ):
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)

    def test_serial_error(self):
        result = self.run_cli("--port", "/dev/nonexistent-relay-port", "ff", "init")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Error sending command", result.stderr)


if __name__ == "__main__":
    unittest.main()
