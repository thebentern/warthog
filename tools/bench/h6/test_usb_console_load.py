import contextlib
import io
from pathlib import Path
import runpy
import socket
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, Mock, patch


SCRIPT = Path(__file__).with_name("usb_console_load.py")
BOARD = "WTHG-0272A1F8738D"
DEST = "192.0.2.1"


class Ping:
    def __init__(self, summary="3 packets transmitted, 0 received", *, exited=None):
        self.summary = summary
        self.returncode = exited
        self.output = None
        self.interrupted = False

    def start(self, args, *, stdout, **kwargs):
        self.output = stdout
        if self.returncode is not None:
            self.output.write(self.summary)
        return self

    def poll(self):
        return self.returncode

    def send_signal(self, sig):
        self.interrupted = True
        self.output.write(self.summary)
        self.returncode = 1  # Linux ping with no echo replies

    def wait(self, timeout):
        return self.returncode


class ConsoleLoadTest(unittest.TestCase):
    def run_load(self, sockets, start_ping, boards=(BOARD,)):
        for udp in sockets:
            udp.__enter__.return_value = udp
        common = SimpleNamespace(
            iface=lambda board: "en7",
            name=lambda board: board[-4:],
            at=lambda board, commands: {cmd: ["OK"] for cmd in commands},
            usb_answers=lambda board: True,
        )
        output = io.StringIO()
        with (
            patch.object(sys, "argv", [str(SCRIPT), "commands", "0.2", DEST, *boards]),
            patch.dict(sys.modules, {"h6common": common, "serial": SimpleNamespace()}),
            patch.object(socket, "socket", side_effect=sockets),
            patch.object(socket, "if_nametoindex", return_value=7),
            patch.object(subprocess, "Popen", side_effect=start_ping),
            contextlib.redirect_stdout(output),
            self.assertRaises(SystemExit) as result,
        ):
            runpy.run_path(str(SCRIPT), run_name="__main__")
        return result.exception.code, output.getvalue()

    def test_udp_bind_failure_fails_the_run(self):
        udp = MagicMock()
        udp.setsockopt.side_effect = PermissionError("cannot bind UDP to en7")
        start_ping = Mock(side_effect=Ping().start)

        code, output = self.run_load([udp], start_ping)

        self.assertEqual(code, 1)
        self.assertIn("RESULT FAIL", output)
        self.assertIn("cannot bind UDP to en7", output)
        start_ping.assert_not_called()
        udp.__exit__.assert_called_once()

    def test_udp_send_failure_fails_the_run(self):
        udp = MagicMock()
        udp.sendto.side_effect = OSError("network is unreachable")
        ping = Ping()

        code, output = self.run_load([udp], ping.start)

        self.assertEqual(code, 1)
        self.assertIn("UDP send failed: network is unreachable", output)
        self.assertTrue(ping.interrupted)

    def test_ping_spawn_failure_fails_the_run(self):
        udp = MagicMock()
        start_ping = Mock(side_effect=FileNotFoundError("ping is missing"))

        code, output = self.run_load([udp], start_ping)

        self.assertEqual(code, 1)
        self.assertIn("RESULT FAIL", output)
        self.assertIn("ping is missing", output)
        udp.__exit__.assert_called_once()

    def test_ping_early_exit_fails_the_run(self):
        ping = Ping("ping: invalid interface en7", exited=2)

        code, output = self.run_load([MagicMock()], ping.start)

        self.assertEqual(code, 1)
        self.assertIn("ping exited before the test ended (status 2)", output)
        self.assertIn("invalid interface en7", output)
        self.assertFalse(ping.interrupted)

    def test_ping_must_report_sending_packets(self):
        for summary in ("", "0 packets transmitted, 0 received"):
            with self.subTest(summary=summary):
                code, output = self.run_load([MagicMock()], Ping(summary).start)

                self.assertEqual(code, 1)
                self.assertIn("ping reported no packets sent", output)

    def test_ping_send_error_fails_even_with_transmitted_count(self):
        for operation in ("sendmsg", "sendto"):
            with self.subTest(operation=operation):
                error = f"ping: {operation}: Network is unreachable"
                ping = Ping(error + "\n3 packets transmitted, 0 received")
                code, output = self.run_load([MagicMock()], ping.start)

                self.assertEqual(code, 1)
                self.assertIn(f"ping send failed: {error}", output)

    def test_sent_traffic_passes_without_echo_replies(self):
        udp = MagicMock()
        ping = Ping()
        start_ping = Mock(side_effect=ping.start)

        code, output = self.run_load([udp], start_ping)

        self.assertEqual(code, 0)
        self.assertIn("RESULT pass", output)
        udp.sendto.assert_any_call(bytes(1200), (DEST, 5001))
        self.assertNotIn("-q", start_ping.call_args.args[0])
        self.assertEqual(start_ping.call_args.kwargs["env"]["LC_ALL"], "C")
        self.assertTrue(ping.interrupted)
        self.assertTrue(ping.output.closed)

    def test_later_setup_failure_cleans_up_earlier_board(self):
        first_udp, second_udp = MagicMock(), MagicMock()
        second_udp.setsockopt.side_effect = PermissionError("cannot bind second board")
        ping = Ping()

        code, output = self.run_load([first_udp, second_udp], ping.start, (BOARD, "WTHG-0272A1F8823D"))

        self.assertEqual(code, 1)
        self.assertIn("823D", output)
        self.assertIn("cannot bind second board", output)
        self.assertTrue(ping.interrupted)
        self.assertTrue(ping.output.closed)
        first_udp.__exit__.assert_called_once()
        second_udp.__exit__.assert_called_once()
