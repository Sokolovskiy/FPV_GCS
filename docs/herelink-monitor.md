# MAVLink discovery and raw joystick monitor (tasks 1–2)

No UART, CRSF, flight control, flight telemetry, Herelink, or systemd service.

Pi Wi-Fi: `192.168.0.139`; Windows Wi-Fi: `192.168.0.161` (checked 2026-11-18).
System ID 42, component ID 1 (QGC discovery compatibility), MAV_TYPE_GENERIC,
MAV_AUTOPILOT_INVALID (not a flight controller), MAV_STATE_UNINIT, mode flags zero.
Heartbeat describes only this software endpoint. Any Disarmed/Ready UI label in
QGC is not a measurement of a downstream FC; none is connected to this software.

HEARTBEAT at 1 Hz, gateway version and a real read-only GW_DISCOVERY=1 parameter
on request, empty gateway mission lists, command ACKs (ARM/DISARM denied).
GPS, battery, position, altitude, attitude and FC ARM telemetry are never sent.
MANUAL_CONTROL addressed to system 42 is logged without scaling or forwarding.
All other control input is ignored. No received message drives hardware.

```sh
cd ~/herelink-fpv-gateway
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python mavlink_server.py --peer 192.168.0.161 --port 14551 --peer-port 14552
```

QGC → Application Settings → Comm Links → Add:

- Name: Pi Discovery
- Type: UDP
- Local Port: 14552
- Server: 192.168.0.139:14551 (click Add Server)
- High Latency: off; automatic connection on start: off
- Save, then Connect

14550 remains QGC's standard auto-connect port. 14552 is a separate explicit
link to avoid port conflicts and establish two-way UDP traffic. Both endpoint
ports and addresses are configurable. No firewall or network adapter changes.

Current run log: `server.task2.log`; task 1 log retained as `server.log`.
Process PID: `server.pid`.
Stop with `kill -TERM "$(cat server.pid)"` after verifying that PID still belongs
to this server. No boot-start or automatic restart is installed.

Discovery is complete only after observing a Vehicle in actual QGC; successful
UDP sends or a simulated GCS alone are insufficient.

## Task 1 verified result — 2026-11-18

Observed in the installed, unmodified QGroundControl Daily 5.2.0:

- Before connection: Disconnected. After connecting Pi Discovery: Ready / PreFlight.
- Analyze Tools → MAVLink Inspector: HEARTBEAT, component 1, actual rate 1.0 Hz;
  received count increases. Fields: type=0, autopilot=8, base_mode=0,
  custom_mode=0, system_status=0, mavlink_version=3.
- Pi logs confirm QGC source 255/190, version request, parameter request and
  mission-list request, with corresponding replies from this system ID 42 endpoint.
- Original es900_crsf.py SHA-256 remains
  e033b155af8e528f9e54b9f25a37926b0672be72ec6d8cba87c8c9dcd29fa9af.
- Running server file descriptors are only stdin, stdout, stderr and UDP socket;
  there is no open UART. No systemd service was created.

The previously enabled QGC joystick can cause incoming control messages; the
discovery server only counts their message types and ignores their contents.
No joystick behavior was implemented or configured for this task.

Task 1 acceptance criterion met: QGC sees a MAVLink Vehicle. Server left running
for inspection; no work on subsequent stages.

## Task 2: raw MANUAL_CONTROL monitor

Every accepted message prints Roll (`y`), Pitch (`x`), Throttle (`z`), Yaw (`r`),
original `buttons` and MAVLink 2 `buttons2`, combined button mask, source/target,
local receipt timestamp with timezone, inter-message gap in milliseconds and
rolling command frequency. Values are not clamped, inverted, or converted.
MANUAL_CONTROL has no sender timestamp; `received_at` is the Pi receipt time.
Frequency uses monotonic time over the most recent one-second window and reports
zero after that window has no samples. One sample is insufficient to estimate Hz.
Every five seconds a summary includes command count, raw axis extrema and button OR-mask.

The original task 1 implementation is preserved as `mavlink_server.task1.py.bak`.
The same system/component IDs, heartbeat identity and network endpoints are retained.
No QGC setting change was required: QGC was already sending MANUAL_CONTROL.

Software checks on Pi:

```sh
.venv/bin/python -m unittest -v test_manual_monitor
```

Three checks passed: original values/buttons/time output, monotonic rate/idle
behavior, actual UDP decode and wrong-target rejection with heartbeat preserved.
An isolated server on localhost ports 14651/14652 received all 50 commands from
`manual_test_client.py`, approximately 24.9 Hz. These test samples are kept in
`software_test.log`, separate from actual QGC data in `server.task2.log`.

Real QGC stick-movement verification is pending user action. In the installed
QGC build, joystick configuration can suspend control transmission; use Fly view
after enabling the joystick. Do not open a second server on port 14551.

References:
[MAVLink heartbeat protocol](https://mavlink.io/en/services/heartbeat.html),
[installed QGC build's discovery logic](https://github.com/mavlink/qgroundcontrol/blob/a76f22939/src/Vehicle/MultiVehicleManager.cc).
