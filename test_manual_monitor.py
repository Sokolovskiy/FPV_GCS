"""Software checks for raw observation, routing and rolling command frequency."""
import socket
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from pymavlink.dialects.v20 import common as mavlink

from mavlink_server import ManualMonitor, RCMonitor, Server, rc_state


class ManualTests(unittest.TestCase):
    def message(self, target=42, x=-1000, y=1000, z=0, r=-500, buttons=5, buttons2=2):
        return mavlink.MAVLink_manual_control_message(target, x, y, z, r, buttons, buttons2)

    def test_original_values_buttons_and_receipt_time_are_logged(self):
        monitor = ManualMonitor()
        with self.assertLogs("discovery", level="INFO") as logs:
            monitor.observe(self.message(), 1.0, "2026-11-18T18:00:00.123+02:00")
        line = next(line for line in logs.output if "MANUAL_CONTROL received_at=" in line)
        for expected in ("roll(y)=1000", "pitch(x)=-1000", "throttle(z)=0",
                         "yaw(r)=-500", "buttons=5 buttons2=2", "combined=0x00020005",
                         "received_at=2026-11-18T18:00:00.123+02:00", "gap_ms=first"):
            self.assertIn(expected, line)
        # Sentinel/negative values must stay raw rather than being clamped or converted.
        monitor.observe(self.message(x=32767, z=-1000), 1.04, "next")
        self.assertEqual(monitor.ranges["pitch"], [-1000, 32767])
        self.assertEqual(monitor.ranges["throttle"], [-1000, 0])

    def test_frequency_uses_monotonic_window_and_returns_zero_when_idle(self):
        monitor = ManualMonitor()
        with patch("mavlink_server.LOG"):
            for index in range(26):
                monitor.observe(self.message(), 10 + index * .04, "wall time may change")
        self.assertAlmostEqual(monitor.frequency(11), 25, places=6)
        self.assertEqual(monitor.frequency(12.01), 0)
        self.assertEqual(monitor.count, 26)

    def test_real_udp_packet_and_wrong_target_preserve_heartbeat(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(1)
            server = Server("127.0.0.1", 0, "127.0.0.1", peer.getsockname()[1], 42)
            try:
                class Writer:
                    def write(self, data):
                        return peer.sendto(data, server.socket.getsockname())
                sender = mavlink.MAVLink(Writer(), srcSystem=255, srcComponent=190)
                import select
                # Consume each datagram before sending the next; UDP may drop bursts.
                for message in (self.message(), self.message(target=99)):
                    sender.send(message)
                    self.assertTrue(select.select([server.socket], [], [], 1)[0])
                    server.receive()
                self.assertEqual(server.manual.count, 1)
                self.assertEqual(server.rx["manual_wrong_target"], 1)
                server.heartbeat()
                messages = mavlink.MAVLink(None).parse_buffer(peer.recv(4096))
                heartbeat = messages[0]
                self.assertEqual(heartbeat.get_type(), "HEARTBEAT")
                self.assertEqual(heartbeat.get_srcSystem(), 42)
                self.assertEqual(heartbeat.get_srcComponent(), 1)
                self.assertEqual(heartbeat.autopilot, mavlink.MAV_AUTOPILOT_INVALID)
            finally:
                server.socket.close()

    def test_extension_flags_distinguish_zero_from_disabled(self):
        monitor = ManualMonitor()
        message = self.message()
        message.s = -123
        message.aux1 = 0
        message.aux6 = 1000
        message.enabled_extensions = 0x84  # aux1 and aux6, but not s
        with self.assertLogs("discovery", level="INFO") as logs:
            monitor.observe(message, 1, "now")
        self.assertEqual(monitor.extension_ranges, {"aux1": [0, 0], "aux6": [1000, 1000]})
        self.assertIn('"s":-123', " ".join(logs.output))
        self.assertIn("enabled_extensions=0x84", " ".join(logs.output))

    def test_rc_all_channels_and_sentinel_semantics(self):
        message = mavlink.MAVLink_rc_channels_override_message(
            42, 1, 65535, 0, 1500, 1000, 1000, 1500, 2000, 65535,
            0, 65534, 65535, 1200, 1300, 1400, 1500, 1600, 1700, 1800)
        decoded = mavlink.MAVLink(None).parse_buffer(message.pack(mavlink.MAVLink(None)))[0]
        monitor = RCMonitor()
        with self.assertLogs("discovery", level="INFO") as logs:
            monitor.observe(decoded, 1, "now")
        self.assertEqual(monitor.ranges["ch18"], [1800, 1800])
        for channel in (1, 2, 8, 9, 10, 11):
            self.assertNotIn(f"ch{channel}", monitor.ranges)
        self.assertEqual(rc_state(9, 1500, 1), "absent_mavlink1")
        self.assertEqual(rc_state(1, 0, 2), "release")
        self.assertEqual(rc_state(9, 0, 2), "ignore")
        self.assertEqual(rc_state(9, 65534, 2), "release")
        self.assertIn('"ch18":{"raw":1800,"state":"value"}', logs.output[0])

    def test_json_capture_preserves_decoded_raw_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "inputs.jsonl"
            server = Server("127.0.0.1", 0, "127.0.0.1", 14652, 42, path)
            try:
                message = self.message(buttons2=32768)
                message.enabled_extensions = 4
                message.aux1 = -999
                decoded = mavlink.MAVLink(None).parse_buffer(message.pack(mavlink.MAVLink(None)))[0]
                with patch("mavlink_server.LOG"):
                    server.handle(decoded)
                record = json.loads(path.read_text())
                self.assertEqual(record["fields"]["aux1"], -999)
                self.assertEqual(record["fields"]["buttons2"], 32768)
                self.assertEqual(record["wire_version"], 2)
                self.assertGreater(record["payload_length"], 11)
            finally:
                server.capture.close()
                server.socket.close()


if __name__ == "__main__":
    unittest.main()
