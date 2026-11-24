"""Resource guard only. The existing gateway owns all RC safety decisions."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent


def main():
    mode, = sys.argv[1:]
    if mode not in {'video', 'rc', 'rc-dry-run'}:
        raise RuntimeError('Unknown launch mode')
    if mode == 'rc' and not (Path.home() / '.config/fpv-gcs/physical-crsf-approved').is_file():
        raise RuntimeError('Physical CRSF output blocked: bench approval marker absent')
    runtime = Path(os.environ['XDG_RUNTIME_DIR']) / 'fpv-gcs'
    runtime.mkdir(mode=0o700, exist_ok=True)
    # All managed/manual launches through this wrapper use the same lock.
    lock = os.open(runtime / ('video.lock' if mode == 'video' else 'rc.lock'),
                   os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.set_inheritable(lock, True)
    if mode == 'rc':
        device = Path('/dev/serial0').resolve(strict=True)
        check = subprocess.run(['/usr/bin/fuser', str(device)], capture_output=True, text=True)
        if check.returncode != 1 or check.stdout.strip() or check.stderr.strip():
            raise RuntimeError(f'Refusing busy/uncheckable device {device}: '
                               f'{check.stdout} {check.stderr}')
    if mode == 'video':
        command = ['/usr/bin/python3', '-u', str(ROOT / 'video_capture.py')]
    else:
        command = ['/usr/bin/python3', '-u', str(ROOT / 'rc_receiver.py'),
                   '--dry-run' if mode == 'rc-dry-run' else '--uart',
                   '--baudrate', '420000', '--port', '14600',
                   '--sender', '192.168.144.3', '--timeout-ms', '300', '--log-hz', '1']
        if os.environ.get('FPV_RC_TRACE'):
            command += ['--trace', os.environ['FPV_RC_TRACE']]
    print(f'Starting {mode}: {command}', flush=True)
    os.execv(command[0], command)


if __name__ == '__main__':
    main()
