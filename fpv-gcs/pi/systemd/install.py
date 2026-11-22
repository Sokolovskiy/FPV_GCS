"""Install user units; physical CRSF remains disabled and unapproved."""
from pathlib import Path
import shutil
import subprocess


def run(*args):
    subprocess.run(args, check=True)


root = Path(__file__).resolve().parent
units = Path.home() / '.config/systemd/user'
units.mkdir(parents=True, exist_ok=True)
for name in ('fpv-video.service', 'fpv-rc.service'):
    target = units / name
    if target.exists() and target.read_bytes() != (root / name).read_bytes():
        raise RuntimeError(f'Review existing unit before replacing: {target}')
    shutil.copyfile(root / name, target)
run('systemd-analyze', '--user', 'verify', str(units / 'fpv-video.service'),
    str(units / 'fpv-rc.service'))
run('loginctl', 'enable-linger')
run('systemctl', '--user', 'daemon-reload')
run('systemctl', '--user', 'enable', '--now', 'fpv-video.service')
print('Installed. RC not enabled or started; requires bench confirmation.')
