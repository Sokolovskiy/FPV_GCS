#!/usr/bin/env python3
"""FPV GCS RC/UDP v1 monitor. Dry-run by default; physical CRSF UART requires --uart."""
import argparse
from collections import deque
import socket
import struct
import time
import math
import os
import selectors
import sys
import signal
import json
from contextlib import ExitStack
from rc_watchdog import RcWatchdog, State
from crsf_encoder import DryRunEncoder
from uart_output import UartOutput, UartOutputError, open_serial

PACKET = struct.Struct("!4sBBHIIQ16H")


def receive_datagram(sock, timestamp_option=None):
    """On 64-bit Linux, reject data that sat in the kernel RX queue >60 ms.

    Realtime is used only to measure queue residence against the kernel timestamp;
    watchdog deadlines and recovery qualification remain monotonic. A clock step
    conservatively discards an affected packet instead of authorizing old input.
    """
    if timestamp_option is None:
        data, source = sock.recvfrom(65535)
        return data, source, True
    data, ancillary, flags, source = sock.recvmsg(65535, socket.CMSG_SPACE(16))
    fresh = False
    for level, kind, value in ancillary:
        if level == socket.SOL_SOCKET and kind == timestamp_option and len(value) >= 16:
            sec, ns = struct.unpack('@ll', value[:16])
            age = time.time_ns() - (sec * 1000000000 + ns)
            fresh = 0 <= age <= 60000000
    return data, source, fresh and not (flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC))


def decode(data):
    if len(data) != PACKET.size:
        raise ValueError("packet length")
    magic, version, count, size, session, seq, stamp, *channels = PACKET.unpack(data)
    if magic != b"FRC1" or version != 1 or count != 16 or size != PACKET.size:
        raise ValueError("header/version/count/size")
    if any(not 1000 <= value <= 2000 for value in channels):
        raise ValueError("channel range")
    return session, seq, stamp, channels


class Monitor:
    def __init__(self, timeout=0.300, *, auto_recovery=True):
        self.watchdog = RcWatchdog(timeout, auto_recovery=auto_recovery)
        self.source = self.session = self.sequence = self.stamp = None
        self.last = None
        self.last_channels = None  # Diagnostic history only. Never a command source.
        self.accepted = self.lost = self.invalid = self.duplicate = self.reordered = 0
        self.sessions = 0
        self.retired = set()
        self.arrivals = deque()
        self.gaps = 0
        self.last_gap = None

    def receive(self, data, source, now):
        # Expire even when input is malformed or the stream resumes after a long gap.
        self.watchdog.poll(now)
        try:
            session, seq, stamp, channels = decode(data)
        except ValueError:
            self.invalid += 1
            self.watchdog.reset_qualification()
            return False
        identity = (source, session)
        current = (self.source, self.session)
        if self.source is not None and source != self.source:
            self.invalid += 1
            return False
        if identity != current:
            if identity in self.retired:
                self.reordered += 1
                self.watchdog.reset_qualification()
                return False
            if self.session is not None:
                self.retired.add(current)
            self.source, self.session = source, session
            self.sequence = self.stamp = None
            self.sessions += 1
            self.watchdog.begin_recovery()
        consecutive = self.sequence is None
        if self.sequence is not None:
            delta = (seq - self.sequence) & 0xffffffff
            if delta == 0:
                self.duplicate += 1
                self.watchdog.reset_qualification()
                return False
            if (delta >= 0x80000000 or stamp < self.stamp
                    or (self.watchdog.auto_recovery and stamp == self.stamp)):
                self.reordered += 1
                self.watchdog.reset_qualification()
                return False
            consecutive = delta == 1 and 0 < stamp - self.stamp <= 60000
            self.lost += delta - 1
        if self.last is not None and self.stamp is not None and now - self.last > self.watchdog.timeout:
            self.gaps += 1
            self.last_gap = {"receive_ms": (now - self.last) * 1000,
                             "sender_ms": (stamp - self.stamp) / 1000,
                             "sequence_delta": (seq - self.sequence) & 0xffffffff}
        self.sequence, self.stamp = seq, stamp
        self.last_channels, self.last = channels, now
        self.watchdog.observe_valid(channels, now, consecutive=consecutive)
        self.accepted += 1
        self.arrivals.append(now)
        self.prune(now)
        return True

    def prune(self, now):
        while self.arrivals and now - self.arrivals[0] > 2.0:
            self.arrivals.popleft()

    def status(self, now):
        self.prune(now)
        hz = ((len(self.arrivals) - 1) / (now - self.arrivals[0])) if len(self.arrivals) > 1 and now > self.arrivals[0] else 0.0
        age = None if self.last is None else (now - self.last) * 1000
        state = self.watchdog.poll(now).value
        freshness = "WAIT" if age is None else "STALE" if age > self.watchdog.timeout * 1000 else "FRESH"
        age_text = "n/a" if age is None else f"{age:.1f}ms"
        values = " ".join(f"last_CH{i}={v}" for i, v in enumerate(self.last_channels or [], 1))
        gate = "ENABLED" if self.watchdog.commands(now) is not None else "BLOCKED"
        return (f"{state} freshness={freshness} commands={gate} hz={hz:.2f} age_rx={age_text} accepted={self.accepted} lost={self.lost} "
                f"invalid={self.invalid} duplicate={self.duplicate} reordered={self.reordered} "
                f"sessions={self.sessions} seq={self.sequence} recovery_count={self.watchdog.recovery_count} {values}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=14600)
    parser.add_argument("--log-hz", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=0, help="seconds; 0 means until Ctrl+C")
    parser.add_argument("--sender", help="optional allowed sender IPv4 address")
    parser.add_argument("--timeout-ms", type=float, default=300, help="watchdog timeout, default 300 ms")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="memory only (default)")
    mode.add_argument("--uart", action="store_true", help="explicit physical CRSF output: /dev/serial0, 50 Hz")
    parser.add_argument("--baudrate", type=int, choices=(400000, 420000), default=420000,
                        help="Pi-to-TX UART: 400000 matches the verified handset setting; 420000 remains the compatibility default")
    parser.add_argument("--control-stdin", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--trace", help="optional JSONL timing trace; never enables UART")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0.1 <= args.log_hz <= 10 or args.duration < 0:
        parser.error("invalid port, log rate or duration")
    if not math.isfinite(args.timeout_ms) or args.timeout_ms <= 0:
        parser.error("timeout-ms must be finite and positive")
    if args.control_stdin and os.name == "nt":
        parser.error("control-stdin requires a POSIX pipe/terminal")
    monitor = Monitor(args.timeout_ms / 1000)
    encoder = DryRunEncoder(monitor.watchdog)
    output = None
    def terminate(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, selectors.DefaultSelector() as selector, ExitStack() as resources:
        trace = resources.enter_context(open(args.trace, 'a', buffering=1)) if args.trace else None
        timestamp_option = None
        if sys.platform.startswith('linux') and struct.calcsize('@l') == 8:
            timestamp_option = 35  # Linux SO_TIMESTAMPNS_OLD, 64-bit struct timespec.
            sock.setsockopt(socket.SOL_SOCKET, timestamp_option, 1)
        sock.bind((args.bind, args.port))
        sock.setblocking(False)
        selector.register(sock, selectors.EVENT_READ, "udp")
        control_buffer = b""
        if args.control_stdin:
            selector.register(sys.stdin, selectors.EVENT_READ, "control")
        if args.uart:
            output = resources.enter_context(UartOutput(monitor.watchdog, factory=lambda: open_serial(args.baudrate)))
            print(f"UART opened /dev/serial0 {args.baudrate} baud 8N1; 50 Hz; waiting for stable stream", flush=True)
        start = next_log = time.monotonic()
        print(f"Listening {args.bind}:{args.port}, RC/UDP v1, {PACKET.size} bytes; age_rx = time since last accepted packet", flush=True)
        print(f"Watchdog timeout={args.timeout_ms:g}ms; startup FAILSAFE; automatic recovery: >=10 consecutive packets, >=160ms, gaps <=60ms", flush=True)
        if not output:
            print(f"CRSF DRY-RUN memory only; 50 Hz; baud={args.baudrate}; UART never opened", flush=True)
        previous_state = monitor.watchdog.poll(start)
        previous_gaps = 0
        previous_loop = time.monotonic()
        try:
            while not args.duration or time.monotonic() - start < args.duration:
                wait = min(0.02, monitor.watchdog.timeout / 4)
                if monitor.watchdog.last_valid is not None and monitor.watchdog.poll() != State.FAILSAFE:
                    wait = min(wait, max(0, monitor.watchdog.last_valid + monitor.watchdog.timeout - time.monotonic()))
                if encoder:
                    wait = min(wait, max(0, encoder.next_due - time.monotonic()))
                for key, _ in selector.select(wait):
                    if key.data == "control":
                        chunk = os.read(sys.stdin.fileno(), 4096)
                        if not chunk:
                            selector.unregister(sys.stdin)
                            control_buffer = b""
                            continue
                        control_buffer += chunk
                        while b"\n" in control_buffer:
                            line, control_buffer = control_buffer.split(b"\n", 1)
                            print("CONTROL rejected: automatic recovery requires stable RC stream", flush=True)
                        if len(control_buffer) > 1024:
                            control_buffer = b""
                    else:
                        try:
                            data, source, fresh = receive_datagram(sock, timestamp_option)
                        except BlockingIOError:
                            continue
                        if not fresh:
                            monitor.invalid += 1
                            monitor.watchdog.reset_qualification()
                            if trace:
                                trace.write(json.dumps(dict(event='reject', time=time.monotonic(),
                                    reason='kernel_queue_age_or_truncation')) + '\n')
                        elif args.sender and source[0] != args.sender:
                            monitor.invalid += 1
                            if trace:
                                trace.write(json.dumps(dict(event='reject', time=time.monotonic(),
                                    reason='sender', source=source[0])) + '\n')
                        else:
                            received_at = time.monotonic()
                            accepted = monitor.receive(data, source[0], received_at)
                            if trace:
                                trace.write(json.dumps(dict(event='packet', time=received_at,
                                    accepted=accepted, session=monitor.session, seq=monitor.sequence,
                                    stamp=monitor.stamp, lost=monitor.lost,
                                    state=monitor.watchdog.poll(received_at).value)) + '\n')
                if output:
                    output.block_if_needed()
                if encoder:
                    frame = encoder.tick()
                    if frame is not None and output:
                        if output.send(frame) and output.sent == 1:
                            print("UART TX started; ACTIVE; 26-byte CRSF frames", flush=True)
                    if frame is not None and encoder.generated == 1:
                        print(f"CRSF first_frame={frame.hex()} bytes={len(frame)}", flush=True)
                now = time.monotonic()
                loop_ms = (now - previous_loop) * 1000
                if loop_ms > args.timeout_ms:
                    print(f"LOOP DELAY elapsed_ms={loop_ms:.1f}", flush=True)
                previous_loop = now
                if monitor.gaps != previous_gaps:
                    gap = monitor.last_gap
                    print(f"UDP GAP receive_ms={gap['receive_ms']:.1f} sender_ms={gap['sender_ms']:.1f} sequence_delta={gap['sequence_delta']}", flush=True)
                    previous_gaps = monitor.gaps
                state = monitor.watchdog.poll(now)
                if state != previous_state:
                    if trace:
                        trace.write(json.dumps(dict(event='state', time=now, state=state.value,
                            age_ms=None if monitor.last is None else (now-monitor.last)*1000,
                            commands=monitor.watchdog.commands(now) is not None)) + '\n')
                    gate = "ENABLED" if monitor.watchdog.commands(now) is not None else "BLOCKED"
                    print(f"STATE {previous_state.value} -> {state.value} commands={gate}", flush=True)
                    previous_state = state
                if now >= next_log:
                    print(monitor.status(now), flush=True)
                    if encoder:
                        print(f"CRSF {'uart' if output else 'dry_run'} frames={encoder.generated} skipped_slots={encoder.skipped_slots}", flush=True)
                    if output:
                        print(f"UART tx_frames={output.sent}", flush=True)
                    next_log = now + 1 / args.log_hz
        except KeyboardInterrupt:
            pass
        print("FINAL " + monitor.status(time.monotonic()), flush=True)
        if encoder:
            span = (encoder.last_frame_at - encoder.first_frame_at) if encoder.generated > 1 else 0
            hz = (encoder.generated - 1) / span if span > 0 else 0
            print(f"FINAL CRSF {'uart' if output else 'dry_run'} frames={encoder.generated} span_hz={hz:.3f} skipped_slots={encoder.skipped_slots}", flush=True)
        if output:
            print(f"FINAL UART tx_frames={output.sent}", flush=True)
    if output:
        print("UART closed", flush=True)


if __name__ == "__main__":
    try:
        main()
    except UartOutputError as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(2)
