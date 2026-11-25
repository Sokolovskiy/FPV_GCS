# Raspberry Pi RC Gateway

**Починаєте з чистої microSD?** Відкрийте
[покрокову інструкцію встановлення для Raspberry Pi 3 Model B](https://github.com/Sokolovskiy/FPV_GCS/blob/main/docs/pi-setup.md).
Вона охоплює ОС, залежності, мережу, UART, відео, RC та автозапуск.
Наведені нижче команди запуску потребують уже налаштованої Pi.

Current configuration: UDP 14600, watchdog 300 ms, UART **420000 baud**, CRSF
**50 Hz**. Channel mapping, 26-byte CRSF framing and es900_crsf.py are unchanged.

Default operation is memory-only:

```sh
python3 -u rc_receiver.py --dry-run --baudrate 420000 --port 14600 --timeout-ms 300 --sender 192.168.144.3
```

Physical output is a separate explicit launch, only after bench readiness:

```sh
python3 -u rc_receiver.py --uart --baudrate 420000 --port 14600 --timeout-ms 300 --sender 192.168.144.3
```

## Automatic recovery

- Startup / >300 ms without accepted packets: FAILSAFE; commands cleared.
- First fresh valid packet: RECOVERY; no CRSF output.
- At least 10 consecutive packets, at least 160 ms receive span, receive gaps
  <=60 ms, sequence delta exactly 1, sender timestamp increasing by <=60 ms:
  ACTIVE automatically. At nominal 50 Hz this normally takes 180 ms.
- Rejected duplicates, reordered, retired-session and malformed packets reset
  RECOVERY progress. Sequence gaps, large timestamp steps and receive gaps >60 ms
  start qualification again from the current valid packet. Unrelated source IPs
  are ignored and never refresh the watchdog deadline.
- A new UDP session immediately blocks existing ACTIVE commands and starts fresh
  qualification. Retired sessions cannot return. A same-session sequence reset is
  rejected; app restart must create a new random session.
- Interrupted RECOVERY expires to FAILSAFE and cannot reuse earlier progress.
- No manual ACK, FC configuration change or ARM override occurs.

Use watchdog.commands(), never diagnostic last_channels. Encoder and UART writer
recheck permission and do not retry cached commands. Blocked output discards queued
bytes. UART faults close the device and terminate; automatic RC recovery does not
reopen failed hardware. The 50 Hz scheduler skips missed slots, with no catch-up
bursts. Timeout uses monotonic age >300 ms and is checked each loop and before
writing; this is not a hard real-time OS guarantee.

64-bit Linux SO_TIMESTAMPNS rejects datagrams queued in the kernel for >60 ms,
including missing/truncated timestamps. Realtime is used only against the kernel
RX timestamp; watchdog/qualification use monotonic time. A clock step may drop
packets conservatively. Other platforms retain sequence/monotonic gates but lack
this kernel queue check. Sender/Pi clocks are unsynchronized, so absolute one-way
network delay is not measured or bounded. The LAN protocol is unauthenticated.

## Tests and deployment

Run `python3 -m unittest -v` here. Task 7 tests exercise the default automatic
policy, real receiver processes without stdin, loss/return, sessions, sequence
reset/wraparound, rejected traffic, interrupted recovery, bursts, kernel queue age
and current AUX/throttle forwarding through a mock UART. Historical isolated tests
explicitly use auto_recovery=False to test individual legacy gates; production
CLI never exposes this mode. The retained acknowledge_recovery() cannot bypass
qualification under the default automatic policy. Deprecated --control-stdin
rejects all control lines.

Deployment: ~/fpv-gcs-udp/. Task 10 adds independent user systemd services and
boot startup; see [systemd/README.md](systemd/README.md) for current control and logs.
The receiver.pid/receiver.log instructions describe historical manual launches.
Task 7 observed dry-run uses observe_recovery.py and records
task7-events.jsonl. Historical Task 3–6 reports remain under verification; their
manual-ACK/400000 instructions are superseded by this document.

## ARM and Betaflight

Gateway forwards current channels without forcing AUX/throttle values. It cannot
infer FC ARM AUX assignments/ranges, Stage 2 action, rescue or arming flags.
Automatic RC recovery is not automatic rearming. Betaflight may resume control in
Stage 1, disarm in Stage 2, or require ARM OFF after RX recovery. Do not change FC
settings to bypass these protections. Read-only FC configuration review and a
disarmed, propeller-free bench test are needed to assess actual FC behavior;
software channel tests alone do not establish safe in-flight recovery.
