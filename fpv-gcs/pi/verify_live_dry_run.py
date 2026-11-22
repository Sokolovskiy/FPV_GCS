"""One-shot live-port test. Requires explicit 'ack' on stdin; opens no UART."""
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import time
from test_crsf_encoder import reference_functions, unpack


def main():
    root = Path(__file__).resolve().parent
    # Abort the test if the child attempts to import serial or open a device.
    guarded = '''import sys, runpy
def guard(event, args):
    if event == 'import' and (args[0] == 'serial' or args[0].startswith('serial.')):
        raise RuntimeError('serial import forbidden in dry run')
    if event == 'open' and isinstance(args[0], (str, bytes)):
        path = args[0].decode() if isinstance(args[0], bytes) else args[0]
        if path.startswith('/dev/') and path != '/dev/null':
            raise RuntimeError('device open forbidden in dry run: ' + path)
sys.addaudithook(guard)
sys.argv = ['rc_receiver.py', '--port', '14600', '--sender', '192.168.0.161', '--dry-run', '--control-stdin', '--duration', '40', '--log-hz', '2']
runpy.run_path('rc_receiver.py', run_name='__main__')
'''
    p = subprocess.Popen([sys.executable, "-u", "-c", guarded], cwd=root,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    incoming = queue.Queue()
    def reader():
        for line in p.stdout:
            lines.append(line)
            incoming.put(line)
            if line.startswith(("STATE ", "ACK ", "CRSF first_frame=", "FINAL ")):
                print(line, end="", flush=True)
    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    try:
        deadline = time.monotonic() + 8
        while True:
            if time.monotonic() > deadline:
                raise AssertionError("no live UDP RECOVERY observed")
            try:
                line = incoming.get(timeout=0.1)
            except queue.Empty:
                continue
            if "STATE FAILSAFE -> RECOVERY" in line:
                break
        assert not any("first_frame=" in line for line in lines)
        print("READY: real UDP received, RECOVERY blocked. Type explicit ack for this dry-run test.", flush=True)
        # The test operator/agent must issue a separate explicit command.
        import select
        if not select.select([sys.stdin], [], [], 25)[0] or sys.stdin.readline().strip() != "ack":
            raise AssertionError("explicit test acknowledgement missing")
        p.stdin.write("ack\n")
        p.stdin.flush()
        time.sleep(2.1)
        devices = [os.readlink(fd) for fd in Path(f"/proc/{p.pid}/fd").iterdir()]
        assert not any(target.startswith("/dev/") and target != "/dev/null" for target in devices), devices
        print("OPEN_FDS " + repr(devices), flush=True)
        p.send_signal(signal.SIGINT)
        p.wait(timeout=3)
        thread.join(timeout=1)
        assert p.returncode == 0
        assert any("ACK accepted state=ACTIVE" in line for line in lines)
        first = next(line for line in lines if "CRSF first_frame=" in line)
        frame = bytes.fromhex(first.split("first_frame=")[1].split()[0])
        raw = unpack(frame)
        assert all(172 <= value <= 1811 for value in raw)
        assert frame == reference_functions()["reference_pack"](raw)
        summary = next(line for line in lines if line.startswith("FINAL CRSF"))
        count = int(summary.split("frames=")[1].split()[0])
        hz = float(summary.split("span_hz=")[1].split()[0])
        assert count >= 90 and 48 <= hz <= 52, summary
        print(f"LIVE PASS channels=16 frames={count} hz={hz:.3f} reference_match=yes UART_open=no", flush=True)
    finally:
        if p.poll() is None:
            p.kill(); p.wait()
        thread.join(timeout=1)
        p.stdin.close(); p.stdout.close()
        (root / "crsf-live-receiver.log").write_text("".join(lines))


if __name__ == "__main__":
    main()
