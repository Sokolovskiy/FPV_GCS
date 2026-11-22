"""Task 7: automatic qualification, real UDP processes and physical-output gate."""
import queue
import socket
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

from rc_receiver import Monitor, receive_datagram
from rc_watchdog import RcWatchdog, State
from crsf_encoder import DryRunEncoder, encode_channels
from uart_output import UartOutput
from test_rc_receiver import packet
from test_uart_output import FakeSerial


class AutoRecoveryTests(unittest.TestCase):
    def test_100hz_keeps_time_qualification_and_300ms_deadline(self):
        m = Monitor()
        for seq in range(10):
            m.receive(packet(seq, stamp=(seq+1)*10000), 'a', 1+seq*.01)
        self.assertIsNone(m.watchdog.commands(1.09))
        for seq in range(10, 18):
            m.receive(packet(seq, stamp=(seq+1)*10000), 'a', 1+seq*.01)
        self.assertEqual(m.watchdog.poll(1.17), State.ACTIVE)
        self.assertIsNone(m.watchdog.commands(1.471))
        # Old sequence/timestamp cannot revive commands after timeout.
        self.assertFalse(m.receive(packet(17, stamp=180000), 'a', 1.48))
        self.assertIsNone(m.watchdog.commands(1.48))
        for seq in range(18, 28):
            m.receive(packet(seq, stamp=(seq+1)*10000), 'a', 2+(seq-18)*.01)
        self.assertIsNone(m.watchdog.commands(2.09))
        for seq in range(28, 36):
            m.receive(packet(seq, stamp=(seq+1)*10000), 'a', 2+(seq-18)*.01)
        self.assertEqual(m.watchdog.poll(2.17), State.ACTIVE)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux kernel RX timestamps')
    def test_old_kernel_queued_udp_is_not_relabelled_fresh(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rx, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
            rx.setsockopt(socket.SOL_SOCKET, 35, 1)
            rx.bind(('127.0.0.1', 0)); rx.settimeout(1)
            tx.sendto(packet(), rx.getsockname())
            time.sleep(.08)
            self.assertFalse(receive_datagram(rx, 35)[2])
            tx.sendto(packet(), rx.getsockname())
            self.assertTrue(receive_datagram(rx, 35)[2])

    def test_auto_wraparound_and_invalid_packets_cannot_refresh_deadline(self):
        m = Monitor()
        sequences = [0xfffffffc, 0xfffffffd, 0xfffffffe, 0xffffffff, 0, 1, 2, 3, 4, 5]
        for i, seq in enumerate(sequences):
            self.assertTrue(m.receive(packet(seq, stamp=(i+1)*20000), 'a', i*.02))
        self.assertEqual(m.watchdog.poll(.18), State.ACTIVE)
        for seq in range(6, 26):
            self.assertFalse(m.receive(packet(seq, stamp=200000), 'a', .2+(seq-6)*.02))
        self.assertEqual(m.last, .18)
        self.assertEqual(m.watchdog.poll(.6), State.FAILSAFE)

    def stream(self, monitor, start=0.0, seq=0, session=7, count=10, value=1500):
        for i in range(count):
            self.assertTrue(monitor.receive(packet(seq+i, session, (seq+i+1)*20000,
                                                   [value]*16), 'a', start+i*.02))
        return start+(count-1)*.02

    def test_tenth_packet_enables_latest_channels_without_ack(self):
        m = Monitor()
        self.stream(m, count=9)
        self.assertEqual(m.watchdog.poll(.16), State.RECOVERY)
        self.assertIsNone(m.watchdog.commands(.16))
        self.assertFalse(m.watchdog.acknowledge_recovery(.16))
        self.stream(m, start=.18, seq=9, count=1, value=1800)
        self.assertEqual(m.watchdog.poll(.18), State.ACTIVE)
        self.assertEqual(m.watchdog.commands(.18), (1800,)*16)

    def test_stop_resume_new_session_and_recovery_interruption(self):
        m = Monitor()
        self.stream(m)
        self.assertEqual(m.watchdog.poll(.481), State.FAILSAFE)
        self.assertIsNone(m.watchdog.commands(.481))
        self.stream(m, .5, 10, count=5)
        self.assertEqual(m.watchdog.poll(.6), State.RECOVERY)
        self.assertEqual(m.watchdog.poll(.901), State.FAILSAFE)
        self.stream(m, 1, 0, session=8, count=9, value=1100)
        self.assertIsNone(m.watchdog.commands(1.16))
        self.stream(m, 1.18, 9, session=8, count=1, value=1900)
        self.assertEqual(m.watchdog.commands(1.18), (1900,)*16)

    def test_new_session_while_active_blocks_until_requalified(self):
        m = Monitor()
        self.stream(m)
        self.stream(m, .2, 0, session=8, count=1, value=1000)
        self.assertIsNone(m.watchdog.commands(.2))
        self.assertEqual(m.watchdog.recovery_count, 1)
        self.assertFalse(m.receive(packet(10, 7, 220000), 'a', .21))
        self.stream(m, .22, 1, session=8, count=10, value=2000)
        self.assertEqual(m.watchdog.commands(.4), (2000,)*16)

    def test_sequence_reset_without_new_session_is_rejected(self):
        m = Monitor()
        self.stream(m, seq=100)
        for seq in range(10):
            self.assertFalse(m.receive(packet(seq, stamp=(seq+1)*20000), 'a', .2+seq*.02))
        self.assertEqual(m.sequence, 109)
        self.assertEqual(m.watchdog.poll(.481), State.FAILSAFE)
        self.stream(m, .5, 0, session=8)
        self.assertEqual(m.watchdog.poll(.68), State.ACTIVE)

    def test_bad_duplicate_reordered_gap_and_burst_restart_count(self):
        for interruption in ('duplicate', 'reordered', 'invalid', 'gap', 'timegap', 'timestamp'):
            with self.subTest(interruption=interruption):
                m = Monitor()
                self.stream(m, count=5)
                seq = 5
                start = .12
                if interruption == 'duplicate':
                    self.assertFalse(m.receive(packet(4, stamp=100000), 'a', .09))
                elif interruption == 'reordered':
                    self.assertFalse(m.receive(packet(3, stamp=80000), 'a', .09))
                elif interruption == 'invalid':
                    self.assertFalse(m.receive(b'bad', 'a', .09))
                elif interruption == 'gap':
                    seq = 6
                elif interruption == 'timegap':
                    start = .15
                else:
                    self.assertFalse(m.receive(packet(5, stamp=100000), 'a', .1))
                    seq = 6
                self.stream(m, start, seq, count=9)
                end = start+.16
                self.assertIsNone(m.watchdog.commands(end))
                self.stream(m, end+.02, seq+9, count=1)
                self.assertEqual(m.watchdog.poll(end+.02), State.ACTIVE)
        m = Monitor()
        for seq in range(100):
            m.receive(packet(seq, stamp=(seq+1)*20000), 'a', seq*.00001)
        self.assertIsNone(m.watchdog.commands(.001))

    def test_uart_and_encoder_emit_nothing_while_blocked_and_use_fresh_aux(self):
        m = Monitor()
        clock = [0.0]
        m.watchdog.clock = lambda: clock[0]
        encoder = DryRunEncoder(m.watchdog, clock=lambda: clock[0])
        port = FakeSerial()
        with UartOutput(m.watchdog, lambda: port) as output:
            for seq in range(10):
                clock[0] = seq*.02
                channels = [1500]*16
                channels[2] = 1000
                channels[4] = 1000 if seq < 9 else 2000
                m.receive(packet(seq, stamp=(seq+1)*20000, channels=channels), 'a', clock[0])
                frame = encoder.tick()
                if seq < 9:
                    self.assertIsNone(frame)
                    self.assertFalse(output.send(encode_channels(channels)))
                else:
                    self.assertTrue(output.send(frame))
                    self.assertEqual(port.frames[-1], encode_channels(channels))
            clock[0] = .481
            output.block_if_needed()
            self.assertIsNone(encoder.tick())
            self.assertEqual(len(port.frames), 1)
            self.assertEqual(port.resets, 1)
            for seq in range(9):
                clock[0] = .5+seq*.02
                m.receive(packet(seq, session=8, stamp=(seq+1)*20000, channels=[1100]*16), 'a', clock[0])
                self.assertIsNone(encoder.tick())
            clock[0] = .68
            m.receive(packet(9, session=8, stamp=200000, channels=[1200]*16), 'a', .68)
            self.assertTrue(output.send(encoder.tick()))
            self.assertEqual(port.frames[-1], encode_channels([1200]*16))

    def test_real_receiver_process_automatic_stop_restart_no_stdin(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        p = subprocess.Popen([sys.executable, '-u', str(Path(__file__).with_name('rc_receiver.py')),
                              '--bind', '127.0.0.1', '--port', str(port), '--dry-run', '--log-hz', '10'],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        incoming = queue.Queue()
        lines = []
        def read():
            for line in p.stdout:
                lines.append(line)
                incoming.put(line)
        thread = threading.Thread(target=read, daemon=True)
        thread.start()
        def wait_for(fragment):
            deadline = time.monotonic()+4
            while time.monotonic() < deadline:
                try:
                    line = incoming.get(timeout=.1)
                except queue.Empty:
                    continue
                if fragment in line:
                    return line
            self.fail('Missing '+fragment+'\n'+''.join(lines))
        def send_stream(tx, session, count):
            for seq in range(count):
                tx.sendto(packet(seq, session, (seq+1)*20000), ('127.0.0.1', port))
                time.sleep(.02)
        try:
            wait_for('Listening')
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
                send_stream(tx, 7, 20)
                wait_for('STATE RECOVERY -> ACTIVE')
                wait_for('STATE ACTIVE -> FAILSAFE')
                frozen = wait_for('CRSF dry_run frames=')
                frozen = int(frozen.split('frames=')[1].split()[0])
                self.assertEqual(int(wait_for('CRSF dry_run frames=').split('frames=')[1].split()[0]), frozen)
                send_stream(tx, 8, 5)
                wait_for('STATE FAILSAFE -> RECOVERY')
                wait_for('STATE RECOVERY -> FAILSAFE')
                send_stream(tx, 9, 20)
                wait_for('STATE FAILSAFE -> RECOVERY')
                wait_for('STATE RECOVERY -> ACTIVE')
                self.assertGreater(int(wait_for('CRSF dry_run frames=').split('frames=')[1].split()[0]), frozen)
        finally:
            p.terminate()
            p.wait(timeout=4)
            thread.join(timeout=1)
            p.stdout.close()
        self.assertNotIn('ACK accepted', ''.join(lines))


if __name__ == '__main__':
    unittest.main()
