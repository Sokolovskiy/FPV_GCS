import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path
from rc_receiver import Monitor
from rc_watchdog import RcWatchdog, State
from test_rc_receiver import packet


class WatchdogTests(unittest.TestCase):
    def test_startup_requires_explicit_confirmation(self):
        w = RcWatchdog(auto_recovery=False, clock=lambda: 0)
        self.assertEqual(w.poll(), State.FAILSAFE)
        self.assertIsNone(w.commands())
        self.assertFalse(w.acknowledge_recovery())
        w.observe_valid(range(1000, 1960, 60))
        self.assertEqual(w.poll(), State.RECOVERY)
        self.assertIsNone(w.commands())
        self.assertTrue(w.acknowledge_recovery())
        self.assertEqual(w.poll(), State.ACTIVE)
        self.assertEqual(w.commands(), tuple(range(1000, 1960, 60)))

    def test_timeout_boundary_and_command_read_checks_clock(self):
        clock = [0.0]
        w = RcWatchdog(auto_recovery=False, clock=lambda: clock[0])
        w.observe_valid([1500] * 16)
        w.acknowledge_recovery()
        clock[0] = 0.300
        self.assertIsNotNone(w.commands())
        clock[0] = 0.301
        # No status(), receive() or timer invocation is needed to block commands.
        self.assertIsNone(w.commands())
        self.assertEqual(w.poll(), State.FAILSAFE)
        self.assertIsNone(w._candidate)

    def test_full_cycle_and_recovery_timeout(self):
        m = Monitor(auto_recovery=False)
        m.receive(packet(), "a", 0)
        self.assertTrue(m.watchdog.acknowledge_recovery(0))
        self.assertEqual(m.watchdog.poll(0.301), State.FAILSAFE)
        m.receive(packet(1), "a", 0.4)
        self.assertEqual(m.watchdog.poll(0.4), State.RECOVERY)
        self.assertIsNone(m.watchdog.commands(0.4))
        for seq in range(2, 10):
            m.receive(packet(seq), "a", 0.4 + seq * 0.02)
        self.assertEqual(m.watchdog.poll(0.6), State.RECOVERY)
        self.assertFalse(m.watchdog.acknowledge_recovery(0.9))
        self.assertEqual(m.watchdog.poll(0.9), State.FAILSAFE)
        m.receive(packet(10), "a", 1)
        self.assertTrue(m.watchdog.acknowledge_recovery(1))
        self.assertEqual(m.watchdog.poll(1), State.ACTIVE)

    def test_packet_gap_is_detected_before_new_packet_refresh(self):
        m = Monitor(auto_recovery=False)
        m.receive(packet(), "a", 0)
        m.watchdog.acknowledge_recovery(0)
        # No periodic poll during the pause: receive itself must latch the timeout.
        m.receive(packet(1), "a", 1)
        self.assertEqual(m.watchdog.poll(1), State.RECOVERY)
        self.assertIsNone(m.watchdog.commands(1))

    def test_losses_within_deadline_remain_active(self):
        m = Monitor(auto_recovery=False)
        m.receive(packet(0), "a", 0)
        m.watchdog.acknowledge_recovery(0)
        m.receive(packet(5), "a", 0.1)
        self.assertEqual(m.lost, 4)
        self.assertEqual(m.watchdog.poll(0.1), State.ACTIVE)
        self.assertIsNotNone(m.watchdog.commands(0.1))
        self.assertEqual(m.watchdog.poll(0.401), State.FAILSAFE)

    def test_rejected_packets_do_not_keep_commands_alive(self):
        for data, source in [(b"broken", "a"), (packet(0), "a"),
                             (packet(0xffffffff), "a"), (packet(1, stamp=99), "a"),
                             (packet(1), "b")]:
            with self.subTest(data=data, source=source):
                m = Monitor(auto_recovery=False)
                m.receive(packet(0), "a", 0)
                m.watchdog.acknowledge_recovery(0)
                self.assertFalse(m.receive(data, source, 0.2))
                self.assertEqual(m.last, 0)
                self.assertEqual(m.watchdog.poll(0.301), State.FAILSAFE)
                self.assertIsNone(m.watchdog.commands(0.301))
                self.assertFalse(m.receive(data, source, 0.4))
                self.assertEqual(m.watchdog.poll(0.4), State.FAILSAFE)

    def test_timeout_configuration_and_diagnostic_history(self):
        m = Monitor(auto_recovery=False, timeout=0.050)
        m.receive(packet(), "a", 0)
        m.watchdog.acknowledge_recovery(0)
        self.assertEqual(m.watchdog.poll(0.051), State.FAILSAFE)
        self.assertIsNone(m.watchdog.commands(0.051))
        self.assertIsNotNone(m.last_channels)
        self.assertIn("commands=BLOCKED", m.status(0.051))
        self.assertIn("last_CH16=1900", m.status(0.051))
        for timeout in [0, -1, float("inf"), float("nan")]:
            with self.assertRaises(ValueError):
                RcWatchdog(timeout)

    def test_wall_clock_is_not_used(self):
        from unittest.mock import patch
        with patch("time.time", side_effect=AssertionError("wall clock used")):
            w = RcWatchdog(auto_recovery=False)
            w.observe_valid([1500] * 16)
            self.assertTrue(w.acknowledge_recovery())
            self.assertIsNotNone(w.commands())

    def test_real_udp_active_loss_stop_and_explicit_recovery(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rx, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
            rx.bind(("127.0.0.1", 0))
            rx.settimeout(1)
            m = Monitor(auto_recovery=False)

            def deliver(seq):
                tx.sendto(packet(seq), rx.getsockname())
                data, source = rx.recvfrom(1024)
                self.assertTrue(m.receive(data, source[0], time.monotonic()))

            deliver(0)
            self.assertTrue(m.watchdog.acknowledge_recovery())
            time.sleep(0.06)
            deliver(3)  # Two missing packets, still within the 300 ms deadline.
            self.assertEqual(m.lost, 2)
            self.assertEqual(m.watchdog.poll(), State.ACTIVE)
            time.sleep(0.34)
            self.assertEqual(m.watchdog.poll(), State.FAILSAFE)
            self.assertIsNone(m.watchdog.commands())
            deliver(4)
            self.assertEqual(m.watchdog.poll(), State.RECOVERY)
            self.assertIsNone(m.watchdog.commands())
            deliver(5)
            self.assertEqual(m.watchdog.poll(), State.RECOVERY)
            self.assertTrue(m.watchdog.acknowledge_recovery())
            self.assertEqual(m.watchdog.poll(), State.ACTIVE)
            self.assertEqual(m.watchdog.commands(), tuple(range(1000, 1960, 60)))

    def test_receiver_process_ticks_without_packets_or_logging(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process = subprocess.Popen([sys.executable, "-u", str(Path(__file__).with_name("rc_receiver.py")),
                                    "--bind", "127.0.0.1", "--port", str(port), "--duration", "1.5",
                                    "--log-hz", "0.1", "--timeout-ms", "100"],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            self.assertIn("Listening", process.stdout.readline())
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
                # UDP may drop a startup datagram on Windows. Exercise streams,
                # rather than assuming reliable delivery of one initial packet.
                for seq in range(12):
                    tx.sendto(packet(seq, stamp=(seq+1)*20000), ("127.0.0.1", port))
                    time.sleep(0.02)
                time.sleep(0.25)
                for seq in range(12, 15):
                    tx.sendto(packet(seq, stamp=(seq+1)*20000), ("127.0.0.1", port))
                    time.sleep(0.02)
            # Keep a single buffered reader after readline(); communicate() can skip
            # text already prefetched by that reader on Windows.
            output = process.stdout.read()
            process.wait(timeout=4)
            process.stdout.close()
            self.assertEqual(process.returncode, 0, output)
            self.assertIn("STATE FAILSAFE -> RECOVERY", output)
            self.assertIn("STATE ACTIVE -> FAILSAFE", output)
            self.assertGreaterEqual(output.count("STATE FAILSAFE -> RECOVERY"), 2, output)
            self.assertIn("commands=ENABLED", output)
            self.assertIn("FINAL FAILSAFE", output)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    unittest.main()
