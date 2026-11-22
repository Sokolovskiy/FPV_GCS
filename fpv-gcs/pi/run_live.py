"""Operator-invoked physical launch. Stops only this project's prior UART gateway.

No tests, dry-run, timer, rearm command, FC settings or original reference script.
Ctrl+C closes UART through the receiver's normal cleanup.
"""
import argparse
import os
from pathlib import Path
import signal
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baudrate', type=int, choices=(400000, 420000), required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    allowed_dirs = {root, Path.home() / 'fpv-gcs-fast-candidate', Path.home() / 'fpv-gcs-baud-check'}
    stopped = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            command = (entry / 'cmdline').read_bytes().decode().strip('\0').split('\0')
            if not command or not Path(command[0]).name.startswith('python'):
                continue
            if not any(Path(value).name in {'rc_receiver.py', 'observe_recovery.py'} for value in command[1:]):
                continue
            if '--uart' not in command or '--port' not in command:
                continue
            if command[command.index('--port')+1] != '14600':
                continue
            if Path(os.readlink(entry / 'cwd')).resolve() not in allowed_dirs:
                continue
            pid = int(entry.name)
            os.kill(pid, signal.SIGTERM)
            stopped.append((entry, command))
        except (FileNotFoundError, ProcessLookupError):
            continue
    deadline = time.monotonic()+5
    for entry, command in stopped:
        while entry.exists():
            try:
                current = (entry / 'cmdline').read_bytes().decode().strip('\0').split('\0')
            except FileNotFoundError:
                break
            if current != command:  # Exited, zombie, or PID reused; never kill again.
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Previous gateway did not stop; physical launch cancelled')
            time.sleep(.05)
    os.chdir(root)
    print(f'REAL UART launch: {args.baudrate} baud / 50 Hz; no duration limit; Ctrl+C stops output', flush=True)
    os.execv(sys.executable, [sys.executable, '-u', str(root / 'rc_receiver.py'),
                             '--uart', '--baudrate', str(args.baudrate), '--port', '14600',
                             '--sender', '192.168.0.161', '--timeout-ms', '300', '--log-hz', '2'])


if __name__ == '__main__':
    main()
