"""Pi-only integration checks. RC stays dry-run, then stopped and disabled."""
from pathlib import Path
import json
import subprocess
import time


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def ctl(*args):
    return run('systemctl', '--user', *args)


def info(name):
    return dict(line.split('=', 1) for line in ctl('show', name,
                '-p', 'MainPID', '-p', 'NRestarts', '-p', 'ActiveState').splitlines())


def ready(name, old=None):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        data = info(name)
        if data['ActiveState'] == 'active' and int(data['MainPID']) > 0 and data['MainPID'] != old:
            time.sleep(.5)
            if info(name)['MainPID'] == data['MainPID']:
                return data
        time.sleep(.2)
    raise RuntimeError(f'{name} failed to start: {info(name)}')


base = Path.home() / '.config/systemd/user'
rc_override = base / 'fpv-rc.service.d/10-verification.conf'
video_override = base / 'fpv-video.service.d/10-verification.conf'
for path in (rc_override, video_override):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise RuntimeError(f'Review pre-existing override: {path}')
results = {}
enabled = subprocess.run(['systemctl', '--user', 'is-enabled', 'fpv-rc.service'],
                         capture_output=True, text=True)
if enabled.stdout.strip() != 'disabled':
    raise RuntimeError('Stop: RC must be disabled before verification')
try:
    # Do not use a physical device even if an approval file already exists.
    rc_override.write_text('[Unit]\nConditionPathExists=\n[Service]\nExecStart=\n'
                           'ExecStart=/usr/bin/python3 -u %h/fpv-gcs-udp/systemd/launch.py rc-dry-run\n')
    ctl('daemon-reload')
    ctl('start', 'fpv-rc.service')
    for name in ('fpv-rc.service', 'fpv-video.service'):
        before = ready(name)
        other = 'fpv-video.service' if name == 'fpv-rc.service' else 'fpv-rc.service'
        other_pid = info(other)['MainPID']
        ctl('kill', '--kill-whom=main', '--signal=SIGKILL', name)
        after = ready(name, before['MainPID'])
        assert int(after['NRestarts']) > int(before['NRestarts'])
        assert info(other)['MainPID'] == other_pid, 'Other service unexpectedly restarted'
        results[name + '_crash'] = {'before': before, 'after': after, 'other_pid': other_pid}
        ctl('stop', name)
        time.sleep(3.5)
        assert info(name)['ActiveState'] == 'inactive', 'Manual stop restarted'
        ctl('start', name)
        ready(name)
    # A missing camera must retry without affecting RC and recover when restored.
    ctl('stop', 'fpv-video.service')
    video_override.write_text('[Service]\nEnvironment=FPV_CAMERA=/dev/fpv-test-absent-c2u\n')
    ctl('daemon-reload')
    other_pid = info('fpv-rc.service')['MainPID']
    ctl('start', 'fpv-video.service')
    time.sleep(7)
    failed = info('fpv-video.service')
    assert int(failed['NRestarts']) >= 1
    assert info('fpv-rc.service')['MainPID'] == other_pid
    results['simulated_missing_camera'] = failed
    video_override.unlink()
    ctl('daemon-reload')
    results['camera_restored'] = ready('fpv-video.service')
    results['linger'] = run('loginctl', 'show-user', 'sokolovskyi', '-p', 'Linger')
    results['uart_holders'] = subprocess.run(['fuser', '/dev/ttyAMA0'], capture_output=True, text=True).stdout
    assert not results['uart_holders'].strip()
finally:
    ctl('stop', 'fpv-rc.service')
    for path in (rc_override, video_override):
        path.unlink(missing_ok=True)
    ctl('daemon-reload')
    ctl('restart', 'fpv-video.service')
print(json.dumps(results, indent=2))
