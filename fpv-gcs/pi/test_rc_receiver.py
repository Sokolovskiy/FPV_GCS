import unittest
from rc_receiver import PACKET, Monitor, decode


def packet(seq=0, session=7, stamp=100, channels=None):
    return PACKET.pack(b"FRC1", 1, 16, 56, session, seq, stamp, *(channels or range(1000, 1960, 60)))


class Tests(unittest.TestCase):
    def test_wire_and_all_channels(self):
        data = packet(0xffffffff, 0x12345678, 0x0102030405060708)
        self.assertEqual(data[:24].hex(), "465243310110003812345678ffffffff0102030405060708")
        self.assertEqual(decode(data)[3], list(range(1000, 1960, 60)))

    def test_invalid_structure(self):
        data = packet()
        invalid = [data[:-1], data + b"x"]
        for offset, value in [(0, 0), (4, 2), (5, 15), (7, 55)]:
            modified = bytearray(data)
            modified[offset] = value
            invalid.append(modified)
        invalid.append(packet(channels=[999] * 16))
        invalid.append(packet(channels=[2001] * 16))
        for data in invalid:
            with self.assertRaises(ValueError):
                decode(data)

    def test_sequence_wrap_loss_duplicates_and_order(self):
        m = Monitor(auto_recovery=False)
        for seq in [0xfffffffe, 0xffffffff, 0, 3]:
            self.assertTrue(m.receive(packet(seq), "a", 1))
        self.assertEqual(m.lost, 2)
        self.assertFalse(m.receive(packet(3), "a", 2))
        self.assertFalse(m.receive(packet(2), "a", 2))
        self.assertFalse(m.receive(packet(4, stamp=99), "a", 2))
        self.assertEqual((m.accepted, m.duplicate, m.reordered), (4, 1, 2))
        self.assertEqual(m.last, 1)

    def test_restart_sender_and_old_session(self):
        m = Monitor(auto_recovery=False)
        self.assertTrue(m.receive(packet(100), "a", 1))
        self.assertTrue(m.receive(packet(0, session=8, stamp=1), "a", 2))
        self.assertFalse(m.receive(packet(101), "a", 3))
        self.assertFalse(m.receive(packet(1, session=8), "b", 3))
        self.assertEqual((m.sessions, m.lost, m.sequence), (2, 0, 0))

    def test_rate_age_and_stale(self):
        m = Monitor(auto_recovery=False)
        self.assertIn("WAIT", m.status(0))
        for seq in range(101):
            m.receive(packet(seq), "a", seq / 50)
        self.assertIn("hz=50.00", m.status(2))
        self.assertIn("age_rx=0.0ms", m.status(2))
        self.assertIn("STALE", m.status(2.301))
        self.assertIn("hz=0.00", m.status(5))

    def test_gap_diagnostics_distinguish_sender_and_receive_pause(self):
        m = Monitor(auto_recovery=False)
        m.receive(packet(0, stamp=1000000), "a", 0)
        m.receive(packet(1, stamp=1020000), "a", 0.4)
        self.assertEqual(m.gaps, 1)
        self.assertEqual(m.last_gap, {"receive_ms": 400, "sender_ms": 20, "sequence_delta": 1})
        m.receive(packet(2, stamp=1520000), "a", 0.9)
        self.assertEqual(m.gaps, 2)
        self.assertEqual(m.last_gap["sender_ms"], 500)
        m.receive(packet(0, session=8, stamp=10), "a", 2)
        self.assertEqual(m.gaps, 2)  # Different sender clock/session is not compared.


if __name__ == "__main__":
    unittest.main()
