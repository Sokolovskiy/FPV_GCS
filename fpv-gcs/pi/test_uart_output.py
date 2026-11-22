import sys
import types
import unittest
import subprocess
import socket
from pathlib import Path
from unittest.mock import patch
from crsf_encoder import encode_channels
from rc_watchdog import RcWatchdog, State
from uart_output import UartOutput, UartOutputError, NonblockingPort, open_serial


class FakeSerial:
    def __init__(self):
        self.frames = []
        self.closed = False
        self.resets = 0
        self.failure = None
        self.short = False
        self.reset_failure = False
        self.close_failure = False
        self.out_waiting = 0

    def fileno(self):
        return 123

    def write(self, frame):
        if self.failure:
            raise self.failure
        self.frames.append(frame)
        return len(frame) - 1 if self.short else len(frame)

    def reset_output_buffer(self):
        self.resets += 1
        if self.reset_failure:
            raise OSError("reset failed")

    def close(self):
        self.closed = True
        if self.close_failure:
            raise OSError("close failed")


class UartTests(unittest.TestCase):
    def setUp(self):
        self.clock = [0.0]
        self.watchdog = RcWatchdog(auto_recovery=False, clock=lambda: self.clock[0])
        self.port = FakeSerial()

    def activate(self, value=1500):
        self.watchdog.observe_valid([value] * 16)
        self.watchdog.acknowledge_recovery()

    def test_port_configuration(self):
        seen = {}
        def factory(**kwargs):
            seen.update(kwargs)
            return self.port
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=factory)}), patch("uart_output.os.set_blocking") as nonblocking:
            self.assertIs(open_serial().serial, self.port)
            nonblocking.assert_called_once_with(123, False)
        self.assertEqual(seen["port"], "/dev/serial0")
        self.assertEqual(seen["baudrate"], 420000)
        self.assertEqual((seen["bytesize"], seen["parity"], seen["stopbits"]), (8, "N", 1))
        self.assertEqual(seen["write_timeout"], 0)
        self.assertTrue(seen["exclusive"])
        self.assertFalse(seen["rtscts"] or seen["dsrdtr"] or seen["xonxoff"])

    def test_active_only_and_correct_close(self):
        with UartOutput(self.watchdog, lambda: self.port) as output:
            frame = encode_channels([1500] * 16)
            self.assertFalse(output.send(frame))
            self.watchdog.observe_valid([1500] * 16)
            self.assertFalse(output.send(frame))
            self.assertEqual(self.port.frames, [])
            self.watchdog.acknowledge_recovery()
            self.assertTrue(output.send(frame))
            self.assertEqual(self.port.frames, [frame])
        self.assertTrue(self.port.closed)
        self.assertEqual(self.port.resets, 1)
        output.close()  # Idempotent.

    def test_explicit_420000_override_reaches_serial_factory(self):
        seen = {}
        def factory(**kwargs):
            seen.update(kwargs)
            return self.port
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=factory)}), patch("uart_output.os.set_blocking"):
            port = open_serial(420000)
            self.assertEqual(seen["baudrate"], 420000)
            port.close()

    def test_nonblocking_os_write_has_no_post_write_timeout(self):
        port = NonblockingPort(self.port)
        frame = encode_channels([1500] * 16)
        with patch("uart_output.os.write", return_value=26) as write:
            self.assertEqual(port.write(frame), 26)
            write.assert_called_once_with(123, frame)
        with patch("uart_output.os.write", side_effect=BlockingIOError("mock backpressure")):
            with self.assertRaises(BlockingIOError):
                port.write(frame)  # No retry loop.

    def test_backpressure_is_fatal_without_queueing_another_frame(self):
        self.activate()
        output = UartOutput(self.watchdog, lambda: self.port)
        self.port.out_waiting = 26
        with self.assertRaisesRegex(UartOutputError, "backlog"):
            output.send(encode_channels([1500] * 16))
        self.assertEqual(self.port.frames, [])
        self.assertTrue(self.port.closed)
        self.assertIsNone(self.watchdog.commands())

    def test_timeout_and_recovery_do_not_transmit_old_values(self):
        self.activate(1000)
        with UartOutput(self.watchdog, lambda: self.port) as output:
            low = encode_channels([1000] * 16)
            self.assertTrue(output.send(low))
            self.clock[0] = 0.301
            self.assertFalse(output.send(low))
            self.assertEqual(self.watchdog.poll(), State.FAILSAFE)
            self.assertEqual(self.port.resets, 1)
            self.watchdog.observe_valid([2000] * 16)
            self.assertFalse(output.send(low))
            self.assertEqual(len(self.port.frames), 1)
            self.assertEqual(self.watchdog.poll(), State.RECOVERY)
            self.watchdog.acknowledge_recovery()
            self.assertTrue(output.send(encode_channels([2000] * 16)))
            self.assertEqual(len(self.port.frames), 2)

    def test_cached_frame_rejected_after_new_snapshot(self):
        self.activate(1000)
        output = UartOutput(self.watchdog, lambda: self.port)
        self.watchdog.observe_valid([2000] * 16)
        with self.assertRaises(UartOutputError):
            output.send(encode_channels([1000] * 16))
        self.assertEqual(self.port.frames, [])
        self.assertTrue(self.port.closed)
        self.assertEqual(self.watchdog.poll(), State.FAILSAFE)

    def test_open_error_invalidates_commands(self):
        self.activate()
        def failure():
            raise PermissionError("mock denied")
        with self.assertRaisesRegex(UartOutputError, "mock denied"):
            UartOutput(self.watchdog, failure)
        self.assertIsNone(self.watchdog.commands())

    def test_write_timeout_disconnect_and_short_write_are_fatal(self):
        for failure, short in [(TimeoutError("mock timeout"), False), (OSError("mock disconnect"), False), (None, True)]:
            with self.subTest(failure=failure, short=short):
                self.port = FakeSerial()
                self.port.failure, self.port.short = failure, short
                self.activate()
                output = UartOutput(self.watchdog, lambda: self.port)
                with self.assertRaises(UartOutputError):
                    output.send(encode_channels([1500] * 16))
                self.assertTrue(self.port.closed)
                self.assertEqual(output.sent, 0)
                self.assertIsNone(self.watchdog.commands())
                self.watchdog.observe_valid([1500] * 16)
                self.watchdog.acknowledge_recovery()
                with self.assertRaises(UartOutputError):
                    output.send(encode_channels([1500] * 16))

    def test_close_even_when_buffer_reset_fails(self):
        self.activate()
        output = UartOutput(self.watchdog, lambda: self.port)
        self.port.reset_failure = True
        with self.assertRaises(UartOutputError):
            output.close()
        self.assertTrue(self.port.closed)
        self.assertIsNone(self.watchdog.commands())

    def test_fault_when_purging_after_timeout_closes_port(self):
        self.activate()
        output = UartOutput(self.watchdog, lambda: self.port)
        output.send(encode_channels([1500] * 16))
        self.port.reset_failure = True
        self.clock[0] = 0.4
        with self.assertRaises(UartOutputError):
            output.block_if_needed()
        self.assertTrue(self.port.closed)
        self.assertIsNone(self.watchdog.commands())

    def test_close_error_invalidates_commands(self):
        self.activate()
        output = UartOutput(self.watchdog, lambda: self.port)
        self.port.close_failure = True
        with self.assertRaises(UartOutputError):
            output.close()
        self.assertIsNone(self.watchdog.commands())

    def test_exception_in_context_closes_port(self):
        with self.assertRaisesRegex(RuntimeError, "test interrupted"):
            with UartOutput(self.watchdog, lambda: self.port):
                raise RuntimeError("test interrupted")
        self.assertTrue(self.port.closed)

    def test_cli_defaults_to_dry_run_and_reports_mock_open_failure(self):
        # Replace the entire serial module in a child process: never open a device.
        receiver = str(Path(__file__).with_name("rc_receiver.py"))
        wrapper = '''import sys,types,runpy
def denied(**kwargs):
    raise OSError("MOCK UART OPEN DENIED")
sys.modules['serial'] = types.SimpleNamespace(Serial=denied)
receiver = sys.argv[1]
sys.argv = [receiver] + sys.argv[2:]
runpy.run_path(receiver, run_name='__main__')
'''
        for uart in [False, True]:
            with self.subTest(uart=uart):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                command = [sys.executable, "-c", wrapper, receiver, "--bind", "127.0.0.1",
                           "--port", str(port), "--duration", "0.1"]
                if uart:
                    command.append("--uart")
                result = subprocess.run(command, capture_output=True, text=True, timeout=4)
                if uart:
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn("MOCK UART OPEN DENIED", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("DRY-RUN memory only", result.stdout)


if __name__ == "__main__":
    unittest.main()
