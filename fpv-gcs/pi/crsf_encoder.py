"""Pure in-memory CRSF encoder. No serial dependency or physical output."""
import time

BAUD = 420000  # Current gateway configuration; no UART is opened here.
RATE = 50
PERIOD = 1 / RATE


def crc8(data):
    # Identical CRC8-DVB-S2 algorithm to the verified es900_crsf.py.
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (0xD5 if crc & 0x80 else 0)) & 0xFF
    return crc


def rc_to_crsf(value):
    """Normalized GCS endpoints 1000/1500/2000 -> raw CRSF 172/992/1811.

    This is full-travel scaling, not literal microseconds-to-CRSF conversion.
    Rounding is nearest integer, ties upward. No channel remapping/inversion.
    """
    if type(value) is not int or not 1000 <= value <= 2000:
        raise ValueError("RC value must be an integer in 1000..2000")
    return 172 + ((value - 1000) * 1639 + 500) // 1000


def pack_raw(channels):
    channels = tuple(channels)
    if len(channels) != 16 or any(type(v) is not int or not 0 <= v <= 2047 for v in channels):
        raise ValueError("expected sixteen 11-bit integer channels")
    packed = sum(value << (11 * i) for i, value in enumerate(channels)).to_bytes(22, "little")
    body = bytes([0x16]) + packed
    return bytes([0xEE, len(body) + 1]) + body + bytes([crc8(body)])


def encode_channels(channels):
    values = tuple(channels)
    if len(values) != 16:
        raise ValueError("expected sixteen RC channels")
    return pack_raw(rc_to_crsf(value) for value in values)


class DryRunEncoder:
    """50 Hz monotonic scheduler; frames are returned only for immediate inspection.

    Never queues frames, caches commands, or returns an old frame when blocked.
    The caller must service tick independently of UDP packet arrivals.
    """
    def __init__(self, watchdog, clock=time.monotonic):
        self.watchdog = watchdog
        self.clock = clock
        self.next_due = clock()
        self.generated = 0
        self.skipped_slots = 0
        self.first_frame_at = self.last_frame_at = None

    def tick(self, now=None):
        live_clock = now is None
        now = self.clock() if now is None else now
        commands = self.watchdog.commands(now)
        if now < self.next_due:
            return None
        # Keep the original periodic grid. After a delay, skip missed slots;
        # never generate a burst of historical commands to catch up.
        elapsed_slots = int((now - self.next_due) / PERIOD) + 1
        self.next_due += elapsed_slots * PERIOD
        if commands is None:
            return None
        self.skipped_slots += elapsed_slots - 1
        frame = encode_channels(commands)
        # Recheck actual time after packing, including any scheduling stall.
        if self.watchdog.commands(self.clock() if live_clock else now) is None:
            return None
        self.generated += 1
        self.last_frame_at = now
        if self.first_frame_at is None:
            self.first_frame_at = now
        return frame
