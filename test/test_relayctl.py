import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import time
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
            self.assertTrue(select.select([master], [], [], 5)[0])
            self.assertEqual(os.read(master, 100), bytes.fromhex(expected))
            self.assertIn("Sent:", result.stdout)
            self.assertNotIn("Listening", result.stdout)
        finally:
            os.close(slave)
            os.close(master)

    def test_listen(self):
        master, slave = pty.openpty()
        process = None
        try:
            process = subprocess.Popen(
                [sys.executable, str(SCRIPT), "--port", os.ttyname(slave), "--listen", "ff", "init"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertTrue(select.select([master], [], [], 5)[0])
            self.assertEqual(os.read(master, 100), bytes.fromhex("ff 02 00 00 00 00 fd"))
            os.write(master, bytes.fromhex("00 80 ff"))
            output = b""
            deadline = time.monotonic() + 5
            while output.count(b"Received: ") < 3:
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, output)
                self.assertTrue(select.select([process.stdout], [], [], remaining)[0], output)
                chunk = os.read(process.stdout.fileno(), 1024)
                self.assertTrue(chunk, output)
                output += chunk
            for byte in ("00", "80", "ff"):
                self.assertIn(f"Received: {byte}\n".encode(), output)
            process.send_signal(signal.SIGINT)
            remaining_output, error = process.communicate(timeout=5)
            output += remaining_output
            self.assertEqual(process.returncode, 0, error)
            self.assertIn(b"Sent:", output)
            self.assertIn(b"Stopped listening.", output)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()
            os.close(slave)
            os.close(master)

    def test_commands(self):
        for address in ("ff", "FF", "0xff", "255", "broadcast"):
            with self.subTest(address=address):
                self.assert_frame("ff 02 00 00 00 00 fd", address, "init")
        self.assert_frame("00 02 00 00 00 00 02", "0", "init")
        self.assert_frame("00 03 00 00 00 00 03", "0", "reset")
        self.assert_frame("ff 03 00 00 00 00 fc", "ff", "reset")
        self.assert_frame("00 04 00 00 00 00 04", "0", "led-off")
        self.assert_frame("ff 04 00 00 00 00 fb", "ff", "led-off")
        self.assert_frame("02 04 00 00 00 00 06", "2", "led-off")
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
            ("reset",),
            ("led-off",),
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
        self.assertIn("Serial error", result.stderr)


if __name__ == "__main__":
    unittest.main()
