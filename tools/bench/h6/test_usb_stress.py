import contextlib
import io
from pathlib import Path
import runpy
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch


SCRIPT = Path(__file__).with_name("usb_stress.py")
WARNING = b"W (12) tusb_desc: unsupported string descriptor index 238\r\n"
SPAM = b"spam 00000042 abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw\r\n"


class UsbStressTest(unittest.TestCase):
    def run_stress(self, duration, chunks):
        now = 0
        incoming = iter(chunks)

        def read(_size):
            nonlocal now
            delay, data = next(incoming)
            now += delay
            return data

        def clock():
            return now

        port = MagicMock()
        port.__enter__.return_value = port
        port.read.side_effect = read
        controller = MagicMock()
        controller.poll.return_value = None
        controller.communicate.return_value = ("", None)
        modules = {
            "serial": SimpleNamespace(Serial=lambda *_args, **_kwargs: port),
            "h6common": SimpleNamespace(port=lambda _serial: "fake-port", tool=lambda name: name, name=lambda s: s),
            "time": SimpleNamespace(monotonic=clock, time=clock),
        }
        output = io.StringIO()
        with patch.dict(sys.modules, modules), patch.object(sys, "argv", [str(SCRIPT), "board", str(duration), "bad"]):
            with patch("subprocess.Popen", return_value=controller), contextlib.redirect_stdout(output):
                with self.assertRaises(SystemExit) as exited:
                    runpy.run_path(str(SCRIPT), run_name="__main__")
        return exited.exception.code, output.getvalue(), controller, port

    def test_warning_only_run_fails_even_before_spam_timeout(self):
        status, output, _, _ = self.run_stress(2, [(1, WARNING)] * 2)
        self.assertEqual(status, 1)
        self.assertIn("NO DIAGNOSTIC SPAM", output)
        self.assertIn("0 diagnostic spam lines", output)

    def test_warnings_do_not_hide_spam_stopping(self):
        status, output, controller, port = self.run_stress(8, [(1, SPAM)] + [(1, WARNING)] * 7)
        self.assertEqual(status, 1)
        self.assertIn("NO DIAGNOSTIC SPAM for 4.0s", output)
        self.assertIn("1 diagnostic spam lines", output)
        controller.terminate.assert_called_once()
        controller.communicate.assert_called_once_with(timeout=2)
        port.__exit__.assert_called_once()

    def test_complete_spam_lines_across_serial_reads_pass(self):
        max_counter = SPAM.replace(b"00000042", b"4294967295")
        chunks = [(1, WARNING + SPAM[:20]), (1, SPAM[20:-1]), (1, SPAM[-1:] + WARNING + max_counter)]
        status, output, _, _ = self.run_stress(3, chunks)
        self.assertEqual(status, 0)
        self.assertIn("2 diagnostic spam lines", output)

    def test_incomplete_and_embedded_spam_are_ignored(self):
        oversized_line = b"W" * 300
        chunks = [(1, oversized_line), (1, SPAM), (1, SPAM[:-1])]
        status, output, _, _ = self.run_stress(3, chunks)
        self.assertEqual(status, 1)
        self.assertIn("0 diagnostic spam lines", output)


if __name__ == "__main__":
    unittest.main()
