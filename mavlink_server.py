"""MAVLink discovery and raw MANUAL_CONTROL monitor. No control or hardware I/O."""
import argparse
from collections import Counter, deque
from datetime import datetime
import logging
import json
import select
import signal
import socket
import time

from pymavlink.dialects.v20 import common as mavlink

LOG = logging.getLogger("discovery")


class ManualMonitor:
    """Observe original values only; timestamps describe local receipt, not sender age."""

    def __init__(self):
        self.times = deque(maxlen=4096)
        self.count = 0
        self.last = None
        self.ranges = {}
        self.buttons_seen = 0
        self.buttons_last = None
        self.extension_ranges = {}

    def frequency(self, now):
        while self.times and now - self.times[0] > 1.0:
            self.times.popleft()
        if len(self.times) < 2:
            return 0.0
        span = self.times[-1] - self.times[0]
        return (len(self.times) - 1) / span if span > 0 else 0.0

    def observe(self, msg, now, received_at):
        gap_ms = None if self.last is None else (now - self.last) * 1000
        self.last = now
        self.times.append(now)
        self.count += 1
        axes = {"roll": msg.y, "pitch": msg.x, "throttle": msg.z, "yaw": msg.r}
        for name, value in axes.items():
            limits = self.ranges.setdefault(name, [value, value])
            limits[:] = [min(limits[0], value), max(limits[1], value)]
        buttons2 = getattr(msg, "buttons2", 0)
        combined = msg.buttons | (buttons2 << 16)
        self.buttons_seen |= combined
        if combined != self.buttons_last:
            LOG.info("BUTTON_STATE received_at=%s previous=%s current=0x%08x active_bits=%s",
                     received_at, "first" if self.buttons_last is None else f"0x{self.buttons_last:08x}",
                     combined, [bit for bit in range(32) if combined & (1 << bit)])
            self.buttons_last = combined
        enabled = getattr(msg, "enabled_extensions", 0)
        extra = {name: getattr(msg, name, 0) for name in ("s", "t", "aux1", "aux2", "aux3", "aux4", "aux5", "aux6")}
        for bit, (name, value) in enumerate(extra.items()):
            if enabled & (1 << bit):
                limits = self.extension_ranges.setdefault(name, [value, value])
                limits[:] = [min(limits[0], value), max(limits[1], value)]
        LOG.info("MANUAL_CONTROL received_at=%s source=%s/%s target=%s "
                 "roll(y)=%s pitch(x)=%s throttle(z)=%s yaw(r)=%s "
                 "buttons=%s buttons2=%s combined=0x%08x gap_ms=%s rate_hz=%.2f "
                 "enabled_extensions=0x%02x extra_raw=%s",
                 received_at, msg.get_srcSystem(), msg.get_srcComponent(), msg.target,
                 msg.y, msg.x, msg.z, msg.r, msg.buttons, buttons2, combined,
                 "first" if gap_ms is None else f"{gap_ms:.2f}", self.frequency(now),
                 enabled, json.dumps(extra, separators=(",", ":")))


def rc_state(channel, value, wire_version):
    if channel > 8 and wire_version == 1:
        return "absent_mavlink1"
    if value == 65535 or (channel > 8 and value == 0):
        return "ignore"
    if (channel <= 8 and value == 0) or (channel > 8 and value == 65534):
        return "release"
    return "value"


class RCMonitor(ManualMonitor):
    def observe(self, msg, now, received_at):
        gap_ms = None if self.last is None else (now - self.last) * 1000
        self.last = now
        self.times.append(now)
        self.count += 1
        frame = msg.get_msgbuf()
        version = 2 if frame and frame[0] == 0xFD else 1
        channels = {}
        for channel in range(1, 19):
            value = getattr(msg, f"chan{channel}_raw", 0)
            state = rc_state(channel, value, version)
            channels[f"ch{channel}"] = {"raw": value, "state": state}
            if state == "value":
                limits = self.ranges.setdefault(f"ch{channel}", [value, value])
                limits[:] = [min(limits[0], value), max(limits[1], value)]
        LOG.info("RC_CHANNELS_OVERRIDE received_at=%s source=%s/%s target=%s/%s "
                 "wire_version=%s gap_ms=%s rate_hz=%.2f channels=%s",
                 received_at, msg.get_srcSystem(), msg.get_srcComponent(),
                 msg.target_system, msg.target_component, version,
                 "first" if gap_ms is None else f"{gap_ms:.2f}", self.frequency(now),
                 json.dumps(channels, separators=(",", ":")))


class Server:
    def __init__(self, bind, port, peer, peer_port, system_id, capture_path=None):
        self.peer = (socket.gethostbyname(peer), peer_port)
        self.system_id = system_id
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.socket.bind((bind, port))
        except OSError:
            self.socket.close()
            raise
        self.socket.setblocking(False)
        # QGC creates Vehicle only for a heartbeat from component 1.
        # INVALID honestly identifies this endpoint as not a flight controller.
        self.mav = mavlink.MAVLink(self, srcSystem=system_id,
                                  srcComponent=mavlink.MAV_COMP_ID_AUTOPILOT1)
        self.running = True
        self.tx = Counter()
        self.rx = Counter()
        self.param_name = b"GW_DISCOVERY"
        self.manual = ManualMonitor()
        self.rc = RCMonitor()
        self.capture = None
        if capture_path:
            try:
                self.capture = open(capture_path, "a", encoding="utf-8", buffering=1)
            except OSError:
                self.socket.close()
                raise

    def capture_input(self, msg, received_at, monotonic):
        if self.capture:
            frame = msg.get_msgbuf()
            record = {"received_at": received_at, "monotonic": monotonic,
                      "source_system": msg.get_srcSystem(), "source_component": msg.get_srcComponent(),
                      "sequence": msg.get_seq(), "wire_version": 2 if frame and frame[0] == 0xFD else 1,
                      "payload_length": frame[1] if frame else None, "fields": msg.to_dict()}
            self.capture.write(json.dumps(record, separators=(",", ":")) + "\n")

    def write(self, packet):
        count = self.socket.sendto(packet, self.peer)
        if count != len(packet):
            raise OSError("Incomplete UDP send")
        self.tx["packets"] += 1
        return count

    def heartbeat(self):
        self.mav.heartbeat_send(mavlink.MAV_TYPE_GENERIC,
                                mavlink.MAV_AUTOPILOT_INVALID,
                                0, 0, mavlink.MAV_STATE_UNINIT)
        self.tx["HEARTBEAT"] += 1

    def version(self):
        # Gateway software 0.1.0; zeros mean no supplied board/OS/FC identity.
        self.mav.autopilot_version_send(mavlink.MAV_PROTOCOL_CAPABILITY_MAVLINK2,
                                       0x00010000, 0, 0, 0, bytes(8), bytes(8), bytes(8),
                                       0, 0, 0)

    def parameter(self):
        # A real, read-only gateway property: this process is discovery-only.
        self.mav.param_value_send(self.param_name, 1.0, mavlink.MAV_PARAM_TYPE_REAL32, 1, 0)

    def handle(self, msg):
        kind = msg.get_type()
        self.rx[kind] += 1
        if getattr(msg, "target_system", self.system_id) not in (0, self.system_id):
            return
        if getattr(msg, "target_component", 1) not in (0, 1):
            return
        if kind == "MANUAL_CONTROL":
            if msg.target != self.system_id:
                self.rx["manual_wrong_target"] += 1
                return
            now = time.monotonic()
            received_at = datetime.now().astimezone().isoformat(timespec="milliseconds")
            self.manual.observe(msg, now, received_at)
            self.capture_input(msg, received_at, now)
        elif kind == "RC_CHANNELS_OVERRIDE":
            now = time.monotonic()
            received_at = datetime.now().astimezone().isoformat(timespec="milliseconds")
            self.rc.observe(msg, now, received_at)
            self.capture_input(msg, received_at, now)
        elif kind == "HEARTBEAT" and self.rx[kind] == 1:
            LOG.info("GCS heartbeat source=%s/%s", msg.get_srcSystem(), msg.get_srcComponent())
            self.mav.statustext_send(mavlink.MAV_SEVERITY_NOTICE,
                b"MAVLink monitor only; no FC state or control")
        elif kind == "COMMAND_LONG":
            result = mavlink.MAV_RESULT_UNSUPPORTED
            requested = None
            if msg.command == mavlink.MAV_CMD_COMPONENT_ARM_DISARM:
                result = mavlink.MAV_RESULT_DENIED
            elif msg.command == mavlink.MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES:
                requested = mavlink.MAVLINK_MSG_ID_AUTOPILOT_VERSION
            elif msg.command == mavlink.MAV_CMD_REQUEST_MESSAGE and msg.param1 in (
                    mavlink.MAVLINK_MSG_ID_AUTOPILOT_VERSION, mavlink.MAVLINK_MSG_ID_HEARTBEAT):
                requested = int(msg.param1)
            if requested is not None:
                result = mavlink.MAV_RESULT_ACCEPTED
            self.mav.command_ack_send(msg.command, result, 0, 0,
                                      msg.get_srcSystem(), msg.get_srcComponent())
            if requested == mavlink.MAVLINK_MSG_ID_AUTOPILOT_VERSION:
                self.version()
            elif requested == mavlink.MAVLINK_MSG_ID_HEARTBEAT:
                self.heartbeat()
            LOG.info("command=%s param1=%s result=%s", msg.command, msg.param1, result)
        elif kind == "PARAM_REQUEST_LIST":
            self.parameter()
            LOG.info("Gateway parameter list served")
        elif kind == "PARAM_REQUEST_READ":
            name = msg.param_id.encode() if isinstance(msg.param_id, str) else msg.param_id
            if msg.param_index == 0 or name == self.param_name:
                self.parameter()
        elif kind == "PARAM_SET":
            name = msg.param_id.encode() if isinstance(msg.param_id, str) else msg.param_id
            if name == self.param_name:
                self.parameter()  # Read-only: report unchanged value.
        elif kind == "MISSION_REQUEST_LIST":
            # This gateway has no missions. This is not a report of FC storage.
            self.mav.mission_count_send(msg.get_srcSystem(), msg.get_srcComponent(), 0,
                                        getattr(msg, "mission_type", 0))
            LOG.info("Empty gateway mission list served")
        # No received message causes hardware I/O or flight control.

    def receive(self):
        for _ in range(64):
            try:
                packet, peer = self.socket.recvfrom(4096)
            except BlockingIOError:
                break
            if peer != self.peer:
                self.rx["foreign_peer"] += 1
                continue
            parser = mavlink.MAVLink(None)
            try:
                messages = parser.parse_buffer(packet) or []
            except mavlink.MAVError:
                self.rx["invalid_packet"] += 1
                continue
            for msg in messages:
                self.handle(msg)

    def run(self):
        LOG.info("Discovery and raw monitor system=%s component=1 UDP=%s peer=%s",
                 self.system_id, self.socket.getsockname(), self.peer)
        next_heartbeat = next_report = time.monotonic()
        try:
            while self.running:
                now = time.monotonic()
                if now >= next_heartbeat:
                    self.heartbeat()
                    next_heartbeat = now + 1.0
                if now >= next_report:
                    LOG.info("tx=%s rx=%s", dict(self.tx), dict(self.rx))
                    LOG.info("MANUAL_SUMMARY count=%s rate_hz=%.2f ranges=%s buttons_seen=0x%08x",
                             self.manual.count, self.manual.frequency(now),
                             self.manual.ranges, self.manual.buttons_seen)
                    LOG.info("AUX_SUMMARY enabled_axis_ranges=%s rc_count=%s rc_rate_hz=%.2f rc_value_ranges=%s",
                             self.manual.extension_ranges, self.rc.count, self.rc.frequency(now), self.rc.ranges)
                    next_report = now + 5.0
                if select.select([self.socket], [], [], .1)[0]:
                    self.receive()
        finally:
            self.socket.close()
            if self.capture:
                self.capture.close()
            LOG.info("Discovery server stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=14551)
    parser.add_argument("--peer", required=True, help="QGC computer's IPv4 address")
    parser.add_argument("--peer-port", type=int, default=14552)
    parser.add_argument("--system-id", type=int, default=42)
    parser.add_argument("--capture", help="Append original input fields and receipt metadata as JSONL")
    args = parser.parse_args()
    if not 1 <= args.system_id <= 254:
        parser.error("system-id must be 1..254")
    if not all(1 <= value <= 65535 for value in (args.port, args.peer_port)):
        parser.error("ports must be 1..65535")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        server = Server(args.bind, args.port, args.peer, args.peer_port, args.system_id, args.capture)
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: setattr(server, "running", False))
        server.run()
        return 0
    except OSError as error:
        LOG.error("Server failed: %s", error)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
