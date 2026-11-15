import importlib.util
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "relayctl.py"
SPEC = importlib.util.spec_from_file_location("relayctl", SCRIPT)
relayctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(relayctl)


class RelayCliTests(unittest.TestCase):
    def test_non_listening_gap_after_flush(self):
        serial = mock.MagicMock()
        uart = serial.Serial.return_value.__enter__.return_value
        uart.write.side_effect = len
        events = []
        uart.flush.side_effect = lambda: events.append("flush")
        with mock.patch.dict(sys.modules, {"serial": serial}), \
                mock.patch.object(relayctl.time, "sleep", side_effect=lambda delay: events.append(delay)):
            self.assertIsNone(relayctl.main(["0", "on"]))
        self.assertEqual(events, ["flush", relayctl.INTER_FRAME_GAP_SECONDS])
        self.assertGreaterEqual(relayctl.INTER_FRAME_GAP_SECONDS, 30 / 9600)

    def test_listen_does_not_add_delay(self):
        serial = mock.MagicMock()
        uart = serial.Serial.return_value.__enter__.return_value
        uart.write.side_effect = len
        uart.read.return_value = bytes.fromhex("ff 02 00 00 00 00 fd 01 00 01")
        with mock.patch.dict(sys.modules, {"serial": serial}), \
                mock.patch.object(relayctl.time, "sleep") as sleep:
            self.assertIsNone(relayctl.main(["--listen", "ff", "init"]))
        sleep.assert_not_called()

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
            self.assertEqual(os.read(master, 100), bytes.fromhex(expected + " 00 ff ff"))
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
            self.assertEqual(os.read(master, 100), bytes.fromhex("ff 02 00 00 00 00 fd 00 ff ff"))
            os.write(master, bytes.fromhex("ff 02 00 00 00 00 fd 03 00 03"))
            output, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, error)
            self.assertIn(b"Sent:", output)
            self.assertIn(b"Received: ff 02 00 00 00 00 fd 03 00 03", output)
            self.assertIn(b"Executed: 3; devices after last executor: 0", output)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()
            os.close(slave)
            os.close(master)

    def test_listen_invalid_and_timeout(self):
        for response, error_text in (
            (bytes.fromhex("ff 02 00 00 00 00 fd 03 00 02"), "invalid status XOR"),
            (bytes.fromhex("ff 02 00 00 00 00 fc 03 00 03"), "invalid command XOR"),
            (bytes.fromhex("ff 02 00 00 00 00 fd 00 00 00"), "invalid status fields"),
            (bytes.fromhex("ff 03 00 00 00 00 fc 01 00 01"), "response does not match"),
            (b"", "incomplete response"),
        ):
            with self.subTest(response=response):
                master, slave = pty.openpty()
                process = None
                try:
                    process = subprocess.Popen(
                        [sys.executable, str(SCRIPT), "--port", os.ttyname(slave), "--listen", "ff", "init"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    )
                    self.assertTrue(select.select([master], [], [], 5)[0])
                    self.assertEqual(len(os.read(master, 100)), 10)
                    if response:
                        os.write(master, response)
                    _, error = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn(error_text.encode(), error)
                finally:
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.communicate()
                    os.close(slave)
                    os.close(master)

    def test_listen_no_executor(self):
        master, slave = pty.openpty()
        process = None
        try:
            process = subprocess.Popen(
                [sys.executable, str(SCRIPT), "--port", os.ttyname(slave), "--listen", "2", "on"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertTrue(select.select([master], [], [], 5)[0])
            self.assertEqual(len(os.read(master, 100)), 10)
            os.write(master, bytes.fromhex("01 ff 00 00 00 00 fe 00 ff ff"))
            output, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, error)
            self.assertIn(b"Executed: 0; no device executed the command", output)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()
            os.close(slave)
            os.close(master)

    def test_unicast_and_no_recipient_feedback(self):
        on = relayctl.make_frame(1, 0xff)
        reached = bytes.fromhex("fe ff 00 00 00 00 01 01 01 00")
        self.assertEqual(relayctl.parse_feedback(reached, on), (1, 1))
        unknown = bytes.fromhex("fe ff 00 00 00 00 01 00 ff ff")
        self.assertEqual(relayctl.parse_feedback(unknown, on), (0, 0xff))
        no_recipient = relayctl.make_frame(0xfe, 0xff)
        self.assertEqual(relayctl.parse_feedback(no_recipient, no_recipient), (0, 0xff))
        with self.assertRaisesRegex(ValueError, "does not match"):
            relayctl.parse_feedback(reached, no_recipient)
        wrong_command = bytes.fromhex("fe 00 00 00 00 00 fe 01 01 00")
        with self.assertRaisesRegex(ValueError, "does not match"):
            relayctl.parse_feedback(wrong_command, on)

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
        for address in ("fe", "FE", "0xfe", "254", "none"):
            with self.subTest(address=address):
                self.assert_frame("fe 00 00 00 00 00 fe", address, "off")
        self.assert_frame("fd 00 00 00 00 00 fd", "253", "off")
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
