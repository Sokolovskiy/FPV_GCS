# FPV GCS — RC Channels

Standalone Windows application: Qt 6 Widgets, C++20, CMake, SDL3 Joystick API.
Version 0.1.10 transmits the existing `std::array<uint16_t, 16>` in 1000–2000
over UDP through Herelink to Raspberry Pi at 192.168.144.20:14600, targeting 100 Hz.
The mapping and profiles are unchanged. USB disconnection suppresses all UDP RC packets.
The Pi gateway separately produces CRSF at 50 Hz / 420000 baud when physically approved.
The separate Herelink GCS project is not modified.

## Run

Open `dist/FPV GCS/FPV GCS.exe`. Keep the entire distribution folder together.
No Python, Qt SDK or SDL installation is needed on the target PC.

Press **F11** to toggle fullscreen. Press it again to restore the previous window state.

## Integrated video (task 9)

The left pane receives the existing Pi stream at `192.168.144.3:5602`:

```text
udpsrc address=192.168.144.3 port=5602 reuse=false
  caps="application/x-rtp,media=(string)video,encoding-name=(string)H264,payload=(int)96,clock-rate=(int)90000"
! rtpjitterbuffer latency=30 drop-on-latency=true
! rtph264depay ! h264parse ! decodebin
! d3d11videosink sync=false enable-last-sample=false force-aspect-ratio=true
```

The existing 30 ms RTP jitter-buffer setting is retained. There are no additional
queues, encoders, QImage conversions or CPU frame transfers in the application.
`decodebin` negotiates the installed decoder with Direct3D11 rendering directly
into the Qt widget's native HWND using
[GstVideoOverlay](https://gstreamer.freedesktop.org/documentation/video/gstvideooverlay.html).
GStreamer initialization, bus handling and shutdown run on a separate worker;
RC timers and protocols are unchanged. The divider adjusts the video pane width.
VIDEO CONNECTED means the sink's rendered-frame counter is advancing. NO SIGNAL
appears after one second without rendered frames; this status timeout does not
buffer or delay video. The worker recreates the video receiver after 5 seconds
without rendered frames (10 seconds while waiting for the first frame), and retries
ERROR/EOS after an interruptible 1-second pause. This resets stale RTP/decoder
state after a sender restart without changing the live pipeline's buffering.
UDP silence is reported separately in the status tooltip and video log. The source
still needs to send RTP/H.264 and codec headers/keyframes to the configured address.
Diagnostics: `%LOCALAPPDATA%/FPVGCS/FPV GCS/video.log`.
Close any separate receiver on port 5602 before starting this app.

`build.ps1` takes `-GStreamerRoot` (default `C:\gstreamer\1.0\msvc_x86_64`)
and packages the GStreamer runtime, required plugins, scanner and licenses beside
the executable. Keep the distribution folder together. No Pi changes are needed.
End-to-end latency and visual smoothness must be checked against the standalone
receiver; no latency measurement is claimed.

Windows/SDL currently detects **OpenTX TX16SMK3 Joystick**, USB **1209:4f54**,
**8 axes, 24 button inputs, 0 POV/HAT**. This identifies available inputs, not
which physical control each input represents. No initial channel bindings are guessed.


## RC over UDP (0.1.9)

The destination is `192.168.144.20`, bound to Windows Ethernet `192.168.144.3`; the port defaults to `14600` and can be
changed in the main window. The port persists in local Qt settings (`udp/port`).
Transmission starts automatically while a selected USB controller is connected
and has a valid snapshot. A precise 10 ms Qt timer refreshes the controller,
evaluates the existing mappings, and sends CH1–CH16 without additional mapping,
inversion or scaling. Timer scheduling on Windows is best effort. There is no
catch-up burst after a delayed callback. Windows timer resolution is requested at 1 ms,
with per-process timer throttling disabled to retain timing when occluded. Failed
socket binds/writes retry automatically. A process lock prevents duplicate GCS instances.
The UDP TX counter counts successful
local socket writes, not delivery acknowledgements. Receiver logs verify delivery.
When USB disappears, no safe-value commands or heartbeat packets are transmitted;
the Pi watchdog enters FAILSAFE after the configured timeout (default 300 ms).
Last values are retained for diagnostics only; the current-command API returns None.
Fresh packets enter RECOVERY and automatically enter ACTIVE after at least 10
consecutive advancing packets over at least 160 ms, with receive gaps at most 60 ms.
At 100 Hz the 160 ms minimum remains in force; ten packets alone cannot enable output.
No manual ACK is required. See `pi/README.md` for the watchdog behavior and tests.

Wire format: exactly **56 bytes**, all integers in network byte order (big endian).
Python struct: `!4sBBHIIQ16H`.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `FRC1` magic |
| 4 | 1 | Protocol version: 1 |
| 5 | 1 | Channel count: 16 |
| 6 | 2 | Total packet size: 56 |
| 8 | 4 | Random session ID, new on application start |
| 12 | 4 | Sequence, starting at zero, modulo 2^32 |
| 16 | 8 | Sender monotonic microseconds since sender creation |
| 24 | 32 | Sixteen uint16 values, CH1 through CH16, 1000–2000 |

The receiver validates size, magic, version, count and all channel ranges before
updating its display. It rejects duplicate and backward sequence numbers, handles
uint32 wraparound, counts sequence gaps, and rejects backward sender timestamps.
A new session allows a sender restart without a false loss count. Retired sessions
are rejected. The first accepted sender IP is pinned for the receiver's lifetime;
source ports may change. This is a LAN monitor protocol without authentication or
retransmission. Restart the monitor to accept a different sender IP.

`age_rx` is **time since the latest accepted packet on the Pi**, not one-way network
latency: Windows/Pi monotonic clocks are not synchronized. `hz` is observed reception
frequency over the trailing two seconds, decaying to zero after reception stops.
`lost` counts sequence gaps within sessions; it cannot count packets before the
first reception, after USB stops, or across application restarts. Logging defaults
to 1 Hz, including while no packets arrive. Channels printed during STALE are old.

Receiver on Pi (Python standard library only):

```sh
cd ~/fpv-gcs-udp
python3 rc_receiver.py --port 14600 --sender 192.168.144.3
# Optional: --log-hz 2 --duration 30
python3 -m unittest -v
```

Source is in `pi/rc_receiver.py`. No root access or serial device is used.
Deployment uses independent user systemd units with linger; see `pi/systemd/README.md`.
Task 13 leaves `fpv-rc.service` in dry-run through `task13-dry-run.conf` until transport
tests and fresh physical bench confirmation permit UART. Never run a second receiver.
Diagnostics: `journalctl --user -u fpv-rc.service`; optional `--trace` records JSONL
arrival/sequence/state timing without opening UART. Wi-Fi is not used for RC.

Verification added: C++ packet golden vector, all 16 independent values, real UDP
loopback, no packet/sequence increment while USB is absent, and invalid range
rejection. Python tests cover malformed headers/length/ranges, sequence gaps,
wraparound, duplicates, reordering, restarts, old sessions, age and rate decay.

## Automatic USB recovery (0.1.9)

The selected controller identity is saved independently of its transient device ID.
Disconnect stops UDP as soon as Windows reports removal; native HID also blocks
incomplete or older-than-100-ms axis reports and stale queued Windows messages.
Reconnect checks the exact saved path/identity and descriptor layout, waits for
fresh inputs, and preserves the existing CH1–CH16 profile. An unknown sole
controller is never selected automatically. On a new installation, select the
intended controller once. Reconnects and app restarts then need no selection/ACK.
A changed USB path/layout requires deliberate selection. Assigned unavailable or
ambiguous inputs suppress the entire packet. Existing unassigned defaults are
unchanged; a completely unassigned profile does not transmit.

Devices without unique serials cannot be distinguished from an identical unit
that Windows exposes with the same path and descriptor. This is not cryptographic
authentication. FC behavior must be checked on a propeller-free, disarmed bench.

## Assign controls

Each row is an RC channel: CH1 Roll, CH2 Pitch, CH3 Throttle, CH4 Yaw,
CH5–CH16 AUX1–AUX12. Click **Призначити…** on the desired row.

1. Click **Налаштувати…**, then **Перепризначити рухом…** (or **Розпізнати орган пульта…**).
2. Move the desired stick, regulator or switch. The changed USB axis is selected
   automatically; no endpoint/center captures or calibration steps are required.
3. Check the live preview and click **Застосувати**. The full HID range is retained,
   regardless of the distance or direction used to identify the axis. For the
   recognized 16-channel layout the source is labeled with the radio CH number.

Version 0.1.4 replaces the three-position capture wizard with movement detection.
It ignores small input jitter, retains short movements, observes a 180 ms window
before accepting, and rejects simultaneous changes on multiple axes. Disconnection,
controller changes and axis-count changes cannot commit an assignment. Existing
profiles are retained; recognizing a new source removes the previous endpoint
calibration and keeps the explicit output mode, inversion and safe-value settings.
This movement workflow identifies scalar USB axes; previously saved digital/POV
profiles remain readable. Button-only encodings do not expose full scalar channels.

Version 0.1.1 fixes the original axis-only detection path. A digital switch is
recognized even if the channel initially had the analog type selected. Three
physical positions can be represented by any distinct HID button patterns,
including one-hot encoding of all three positions. Stable unrelated switches are
excluded from the learned pattern. Unknown patterns fall back to the safe value.
Axis-based and POV-based switches are learned by the same physical-position workflow.
Changing multiple analog controls during learning is rejected as ambiguous.
Existing profiles from 0.1.0 remain readable; saved channel assignments are preserved.
Repeated profile saves on Windows now close the old read handle before the atomic
replacement. This fixes the previous Access denied error after the first save;
regression tests perform repeated saves and preserve other device profiles.

**Діагностика…** keeps raw device information separate from the operator's channel
assignment workflow. Device input counts do not impose an eight-channel output limit.

## Defaults and profiles

The right pane uses compact 8-pixel margins. Only RC Level stretches when the
window or divider is resized. Function, channel function and settings keep fixed
widths of 60, 155 and 46 logical pixels respectively.

Version 0.1.6 lets the operator resize every table column by dragging its header
boundary, including the settings icon column. Widths persist across restarts using
local Qt settings. Channel numbers are centered horizontally with no asymmetric
padding. Version 0.1.7 replaces horizontal overflow with coordinated column resizing.

Version 0.1.5 removes the State column, uses gear icons for channel settings,
adds left padding to channel numbers, and fixes the USB source column to a compact
width. The new **Функція каналу** field accepts a user description independently
of both the RC role (Roll/AUX etc.) and the input source. Editing finishes on Enter
or focus loss and saves the text in the controller's profile. Existing profiles
without descriptions remain compatible. Descriptions never affect RC values.

Version 0.1.3 recognizes the explicitly configured sixteen-axis USB layout above
by HID page/usage, independently of transmitter brand and Windows data-index order.
The observed Windows representation placing all sixteen usages on Simulation page 2
is also recognized, provided the complete unique 0x30–0x38 plus seven Sim usages exist.
With a fresh/unassigned profile it automatically maps radio CH1–CH16 to GCS
CH1–CH16, with no repeated physical-position learning. All channels remain scalar
values: both analog and switch outputs pass through from the radio's mixer without
added quantization or inversion. Existing assigned profiles are kept; manual overrides
remain available. Unrecognized layouts still require explicit mapping. The HID usage
sequence is included in the device profile identity to isolate changed USB layouts.
The sixteen-axis descriptor and receipt of all fields are confirmed on the actual
connected transmitter; moving every physical control remains a user validation step.

All channels start unassigned. Default safe values: CH1/2/4=1500;
CH3 and CH5–CH16=1000. ARM is not assigned automatically. Safe values are editable
per channel; inversion never changes a disconnected/conflict safe value.

On USB loss, the channel array immediately uses safe values at the next input poll.
The application polls at 8 ms and refreshes the display at 20 ms; these are timer
targets, not a hard real-time guarantee. Hotplug detection refreshes device lists.
Legacy inputs are read through SDL Joystick APIs; SDL Gamepad mappings are not used.
Version 0.1.2 adds native Windows Raw Input / HID parsing for controllers advertising
more than eight scalar absolute axes. Generic Desktop and Simulation scalar usages
are read dynamically from the HID descriptor, without a transmitter model/VID whitelist
or an eight-axis cap. Relative inputs, battery fields, hats and HID value arrays
are excluded from that scalar-axis backend. Native profiles use a separate identity;
existing SDL profiles are retained. Native and SDL listings are matched by the
physical Windows HID path (case-insensitive), never by shared USB VID/PID alone.
The native full-axis entry replaces its SDL duplicate; legacy entries retain SDL
buttons and profile identities but use the matching HID product name. This keeps
TX16S radios with identical VID/PID distinguishable even if SDL names both MK3.
Unmatched or ambiguous paths remain selectable.

The transmitter must actually expose each desired channel over USB. EdgeTX Classic
advertises only eight axes plus 24 one-bit buttons; this cannot preserve all three
positions of a switch assigned to one of those bits. No missing positions are invented.
For EdgeTX 2.12.4, USB Joystick Advanced can expose sixteen independent scalar axes:
CH1–9 as Axis X/Y/Z/rotX/rotY/rotZ/Slider/Dial/Wheel, CH10–16 as Sim
Ail/Ele/Rud/Thr/Acc/Brk/Steer. These are USB descriptor labels, not automatic RC
assignments. Select Interface Joystick, Circular cutout None, apply changes and
reconnect USB. The radio's mixer must already produce the desired CH1–16 outputs.
Other transmitters can use any equivalent USB mode that exposes the required axes.
Sources: [EdgeTX USB settings](https://manual.edgetx.org/color-radios/model-settings/model-setup/usb-joystick),
[exact 2.12.4 axis types](https://github.com/EdgeTX/edgetx/blob/v2.12.4/radio/src/usb_joystick.h).

Native parsing has been checked on the connected eight-axis transmitter (all scalar
fields received); sixteen axes on the physical radio still require USB reconfiguration
and the user's movement test. `fpv_probe.exe --native` reports the descriptor count
and whether every scalar axis has received a report, independently of SDL.

A firmware-preserving EdgeTX 2.10 layout avoids mixed Axis/Sim
mode: clear the USB assignments, set CH1–6 to Axis X/Y/Z/rotX/rotY/rotZ and
CH7–16 to repeated Axis Slider; leave CH17–26 None. Use Joystick, Circular
cutout None, apply and reconnect USB. The native parser now supports packed HID
usage value arrays via HidP_GetUsageValueArray. Windows on the physical MK2 instead
exposes all sixteen inputs as separate scalar caps, with ten Sliders first and
rotZ through X last. This exact layout maps CH1–16 to input indices 15–0. The
order was verified by passing a synthetic report with sixteen distinct values
through Windows HidP_GetData using the connected radio's preparsed descriptor;
no report was sent to the device. The six scalar plus ten Slider-array variant
is also recognized. Ambiguous layouts do not get guessed assignments. Diagnostics
show recognized channels in CH order alongside the underlying HID input numbers.
Sources: [EdgeTX 2.10.6 USB implementation](https://github.com/EdgeTX/edgetx/blob/v2.10.6/radio/src/usb_joystick.cpp),
[Windows usage value arrays](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_getusagevaluearray).

Profiles save automatically as JSON under `%LOCALAPPDATA%/FPVGCS/FPV GCS/profiles.json`
(the exact path is displayed in the application). They are bound to controller GUID,
USB IDs, input counts, and serial when available; otherwise device path is included.
A different port/path may require remapping if the transmitter provides no serial.
Invalid profiles fail to unassigned safe defaults. Writes use atomic `QSaveFile`.

## Video capture sources and settings

The compact toolbar above the RC channel table contains the active Pi USB capture selector
and **Video Capture Settings** gear. Both selectors share the Pi's active device;
the receiver endpoint remains `192.168.144.3:5602`. The selector remains usable
while the receiver shows NO SIGNAL. Device names and all capture capabilities
come from the Pi, including unavailable devices and live USB changes.

Settings show actual V4L2 formats, resolution/FPS, physical input/TV standards
when supported, image controls with driver ranges/defaults,
negotiated caps, measured RTP bitrate and transport-sink drop counters.
Image controls apply immediately without resetting the scroll position;
**Apply capture format**, inside the Capture Format group, restarts
only the video pipeline. **Restore image defaults** uses driver defaults.

The Pi independently selects a saved preferred device, or the first available
stable USB identity, and waits for late USB connections when none are present.
Loss of analog video does not change USB selection. Settings persist per camera
under `~/.config/fpv-gcs/video-capture.json`. A serialless dongle retains identity
at the same USB port. The Pi video manager must be installed alongside the updated
Windows app; see [Pi installation and API](pi/systemd/README.md).

The standalone build is `dist/FPV GCS/FPV GCS.exe`; an already installed older
Start-menu copy is not replaced by building this checkout.

## Build and verify

Run `build.ps1` in PowerShell. Defaults target installed Qt 6.11.1 MinGW and GCC 13.1.
CMake/Ninja paths can be supplied as parameters. The script downloads the official
SDL 3.4.20 MinGW SDK if absent and verifies pinned SHA-256, builds, runs tests,
and packages Qt/runtime DLLs with `windeployqt`. Qt's MinGW moc rejected the Unicode
workspace output path, so this project's source/SDK copies and build artifacts are
staged in `%LOCALAPPDATA%/FPVGCSBuild`; source files remain in this project.

`%LOCALAPPDATA%/FPVGCSBuild/build/fpv_probe.exe` prints the actual controller inventory as JSON.
`%LOCALAPPDATA%/FPVGCSBuild/build/fpv_tests.exe` tests channel evaluation, inversion, calibration, 2/3-position
switches, POV, conflicts, 16 independent channels, invalid/missing inputs, profile
round-trip/corruption, SDL virtual device reading and disconnection fallback.
The latest run passed 17 QtTest cases (including setup/cleanup), with no failures.
New regression cases cover automatic digital-switch recognition, active-low switches,
arbitrary three-position button patterns, POV learning, and sixteen independent
channels built from eight physical axes plus eight three-position switches.
The runnable folder includes `test-results.txt` and upstream license texts.

Verified locally: standalone packaged executable starts from the Unicode workspace
without Qt SDK on PATH, detects the actual OpenTX device, shows all 16 channel rows
and explicit default values, and exposes the live source settings/preview dialog.
Physical control-to-channel assignments require the user's deliberate input test;
the initial empty profile does not identify sticks/switches by assumed ordering.

Dependency sources: [SDL3 Joystick API](https://wiki.libsdl.org/SDL3/CategoryJoystick),
[Qt CMake](https://doc.qt.io/qt-6/cmake-get-started.html).
Qt and SDL shared libraries retain their respective upstream licenses.
