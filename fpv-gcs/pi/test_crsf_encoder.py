import ast
import hashlib
import os
from pathlib import Path
import queue
import random
import socket
import subprocess
import sys
import threading
import time
import types
import unittest
from unittest.mock import patch

from crsf_encoder import BAUD, RATE, DryRunEncoder, crc8, encode_channels, pack_raw, rc_to_crsf
from rc_receiver import Monitor
from rc_watchdog import RcWatchdog, State
from test_rc_receiver import packet


def unpack(frame):
    if len(frame) != 26 or frame[:3] != b"\xee\x18\x16":
        raise ValueError("frame structure")
    # Independent bit-by-bit decoder, rather than mirroring the encoder's shifts.
    bits = [(byte >> bit) & 1 for byte in frame[3:25] for bit in range(8)]
    return [sum(bits[i * 11 + j] * 2**j for j in range(11)) for i in range(16)]


def reference_functions():
    source = Path.home() / "es900_crsf.py"
    if not source.exists():
        source = Path(__file__).resolve().parents[1] / "verification/crsf/reference-es900_crsf.py"
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != "e033b155af8e528f9e54b9f25a37926b0672be72ec6d8cba87c8c9dcd29fa9af":
        raise AssertionError("verified reference hash changed")
    tree = ast.parse(data)
    # NEVER import/execute the module: its top-level code opens UART.
    # Compile only the two inspected pure function definitions.
    definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in {"crc8", "make_frame"}]
    namespace = {"time": types.SimpleNamespace(monotonic=lambda: 0)}
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(source), "exec"), namespace)
    # Reuse the reference packing/CRC statements with arbitrary raw channel input.
    body = definitions[1].body
    index = next(i for i, node in enumerate(body) if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and node.targets[0].id == "packed")
    function = ast.FunctionDef(name="reference_pack", args=ast.arguments(posonlyargs=[], args=[ast.arg(arg="channels")], kwonlyargs=[], kw_defaults=[], defaults=[]), body=body[index:], decorator_list=[])
    module = ast.fix_missing_locations(ast.Module(body=[function], type_ignores=[]))
    exec(compile(module, str(source), "exec"), namespace)
    return namespace


class EncoderTests(unittest.TestCase):
    def test_endpoints_and_full_input_range(self):
        self.assertEqual((BAUD, RATE), (420000, 50))
        self.assertEqual([rc_to_crsf(v) for v in [1000, 1500, 2000]], [172, 992, 1811])
        values = [rc_to_crsf(v) for v in range(1000, 2001)]
        self.assertEqual(values, sorted(values))
        self.assertEqual(len(set(values)), 1001)
        for value in [999, 2001, 1500.0, True]:
            with self.assertRaises(ValueError):
                rc_to_crsf(value)

    def test_all_sixteen_channels_and_bit_boundaries(self):
        for baseline in [1000, 1500, 2000]:
            for channel in range(16):
                values = [baseline] * 16
                values[channel] = 2000 if baseline != 2000 else 1000
                self.assertEqual(unpack(encode_channels(values)), [rc_to_crsf(v) for v in values])
        raw = [0, 1, 2, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 1024, 172, 1811, 2047]
        self.assertEqual(unpack(pack_raw(raw)), raw)
        for invalid in [[1500] * 15, [1500] * 17, [999] * 16]:
            with self.assertRaises(ValueError):
                encode_channels(invalid)
        for invalid in [[0] * 15, [2048] * 16, [-1] * 16]:
            with self.assertRaises(ValueError):
                pack_raw(invalid)

    def test_crc_golden_vector_and_entire_frame_residue(self):
        self.assertEqual(crc8(b"123456789"), 0xBC)  # CRC8-DVB-S2 check value.
        for value in [1000, 1500, 2000]:
            frame = encode_channels([value] * 16)
            self.assertEqual(crc8(frame[2:]), 0)
            self.assertEqual(frame[1], len(frame) - 2)
            corrupt = bytearray(frame)
            corrupt[10] ^= 1
            self.assertNotEqual(crc8(corrupt[2:]), 0)

    def test_differential_against_verified_es900_script(self):
        ref = reference_functions()
        for stamp in [0, 2]:
            ref["time"].monotonic = lambda: stamp
            raw = [992] * 16
            raw[0] = 992 if stamp == 0 else 1152
            raw[2] = 172
            raw[4:] = [172] * 12
            self.assertEqual(pack_raw(raw), ref["make_frame"]())
        rng = random.Random(400000)
        for _ in range(300):
            raw = [rng.randrange(2048) for _ in range(16)]
            self.assertEqual(pack_raw(raw), ref["reference_pack"](raw))
            rc = [rng.randrange(1000, 2001) for _ in range(16)]
            self.assertEqual(encode_channels(rc), ref["reference_pack"]([rc_to_crsf(v) for v in rc]))

    def test_scheduler_independent_of_arrivals_and_no_catchup_burst(self):
        clock = [0.0]
        w = RcWatchdog(auto_recovery=False, clock=lambda: clock[0])
        encoder = DryRunEncoder(w, clock=lambda: clock[0])
        emitted = []
        # Deliberately irregular updates, slower than output frame generation.
        arrivals = {0, 37, 121, 273, 400, 511, 649, 790, 925}
        for ms in range(1000):
            clock[0] = ms / 1000
            if ms in arrivals:
                w.observe_valid([1000 + ms] * 16)
                if ms == 0:
                    w.acknowledge_recovery()
            frame = encoder.tick()
            if frame is not None:
                emitted.append(clock[0])
        self.assertEqual(len(emitted), 50)
        self.assertLess(max(abs((b - a) - 0.02) for a, b in zip(emitted, emitted[1:])), 0.0011)
        clock[0] = 1.2
        w.observe_valid([1500] * 16)
        self.assertIsNotNone(encoder.tick())
        self.assertIsNone(encoder.tick())
        self.assertGreater(encoder.skipped_slots, 0)

    def test_stop_recovery_ack_and_no_old_commands(self):
        clock = [0.0]
        m = Monitor(auto_recovery=False)
        e = DryRunEncoder(m.watchdog, clock=lambda: clock[0])
        m.receive(packet(channels=[1000] * 16), "a", 0)
        self.assertIsNone(e.tick())  # Initial RECOVERY.
        m.watchdog.acknowledge_recovery(0)
        clock[0] = 0.02
        self.assertEqual(unpack(e.tick()), [172] * 16)
        clock[0] = 0.301
        self.assertIsNone(e.tick())
        self.assertEqual(m.watchdog.poll(clock[0]), State.FAILSAFE)
        self.assertFalse(m.watchdog.acknowledge_recovery(clock[0]))
        m.receive(packet(1, channels=[2000] * 16), "a", 0.4)
        clock[0] = 0.4
        self.assertIsNone(e.tick())
        self.assertEqual(m.watchdog.poll(clock[0]), State.RECOVERY)
        self.assertEqual(e.generated, 1)
        m.watchdog.acknowledge_recovery(clock[0])
        clock[0] = 0.421
        self.assertEqual(unpack(e.tick()), [1811] * 16)
        self.assertEqual(e.generated, 2)

    def test_stall_during_encoding_discards_frame(self):
        clock = [0.0]
        w = RcWatchdog(auto_recovery=False, clock=lambda: clock[0])
        w.observe_valid([1500] * 16)
        w.acknowledge_recovery()
        e = DryRunEncoder(w, clock=lambda: clock[0])
        def stalled(values):
            clock[0] = 0.4
            return encode_channels(values)
        with patch("crsf_encoder.encode_channels", side_effect=stalled):
            self.assertIsNone(e.tick())
        self.assertEqual(e.generated, 0)

    def test_real_clock_50_hz(self):
        w = RcWatchdog(auto_recovery=False)
        w.observe_valid([1500] * 16)
        w.acknowledge_recovery()
        e = DryRunEncoder(w)
        start = time.monotonic()
        emitted = []
        while time.monotonic() - start < 1.01:
            w.observe_valid([1500] * 16)
            if e.tick() is not None:
                emitted.append(time.monotonic())
            time.sleep(0.001)
        measured = (len(emitted) - 1) / (emitted[-1] - emitted[0])
        self.assertAlmostEqual(measured, 50, delta=1)
        print(f"Measured CRSF memory generation: {measured:.3f} Hz; frames={len(emitted)}", flush=True)



if __name__ == "__main__":
    unittest.main()
