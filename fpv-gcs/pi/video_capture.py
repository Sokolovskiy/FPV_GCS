"""Single-owner USB capture service. No RC, UART or flight-control imports.

V4L2 discovery/control uses v4l2-ctl; the existing H264/RTP sender is owned by
GStreamer in this process. HTTP handlers only enqueue work or read snapshots.
"""
import copy
from fractions import Fraction
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import threading
import time
import uuid


def command(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=4,
                            env={**os.environ, 'LC_ALL': 'C'})
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or str(args))
    return result.stdout


def ctl(device, *args):
    return command('v4l2-ctl', '-d', device, *args)


def optional(device, *args):
    try:
        return ctl(device, *args)
    except RuntimeError:
        return ''


def parse_controls(text):
    controls = []
    for line in text.splitlines():
        match = re.match(r'\s*(\w+)\s+0x[\da-f]+\s+\(([^)]+)\)\s*:\s*(.*)', line)
        if match:
            name, kind, tail = match.groups()
            values = {k: int(v, 0) for k, v in re.findall(
                r'\b(min|max|step|default|value)=(-?0x[\da-f]+|-?\d+)', tail)}
            if kind == 'bool':
                values.update(min=0, max=1, step=1)
            elif kind == 'bitmask':
                values.update(min=0, step=1)
            flags = tail.split('flags=', 1)[-1] if 'flags=' in tail else ''
            string_value = re.search(r'value="(.*)"', tail)
            if string_value:
                values['value'] = string_value[1]
            controls.append(dict(name=name, type=kind, **values, flags=flags, menu=[],
                                 writable=not any(f in flags for f in
                                     ('read-only', 'inactive', 'disabled', 'grabbed'))))
        else:
            item = re.match(r'\s+(-?\d+):\s+(.+)', line)
            if item and controls:
                controls[-1]['menu'].append({'value': int(item[1]), 'label': item[2]})
    return controls


def parse_formats(text):
    """Keep discrete tuples and actual stepwise ranges, never invent modes."""
    formats, current, size = [], None, None
    for line in text.splitlines():
        match = re.search(r"\[\d+\]:\s+'([^']+)'", line)
        if match:
            current = {'pixel': match[1], 'sizes': []}
            formats.append(current)
            size = None
        match = re.search(r'Size: Discrete (\d+)x(\d+)', line)
        if match and current is not None:
            size = dict(width=int(match[1]), height=int(match[2]), rates=[])
            current['sizes'].append(size)
        match = re.search(r'Size: (?:Stepwise|Continuous) (\d+)x(\d+) - (\d+)x(\d+)(?: with step (\d+)/(\d+))?', line)
        if match and current is not None:
            a, b, c, d, sx, sy = match.groups()
            size = dict(width=int(a), height=int(b), rates=[], range=dict(
                min_width=int(a), min_height=int(b), max_width=int(c), max_height=int(d),
                step_width=int(sx or 1), step_height=int(sy or 1)))
            current['sizes'].append(size)
        match = re.search(r'Interval: Discrete [\d.]+s \(([\d.]+) fps\)', line)
        if match and size is not None:
            size['rates'].append(match[1])
        match = re.search(r'Interval: (?:Stepwise|Continuous) ([\d.]+)s - ([\d.]+)s(?: with step ([\d.]+)s)?', line)
        if match and size is not None:
            size['interval_range'] = dict(min=float(match[1]), max=float(match[2]),
                                          step=float(match[3] or 0))
    return formats


def choices(text, pattern):
    return [{'value': int(m[0]), 'label': m[1].strip()} for m in re.findall(pattern, text)]


def discover():
    devices = []
    # Prefer serial-based IDs; by-path is stable across /dev/videoN renumbering
    # and distinguishes identical dongles without unique serial numbers.
    aliases = {}
    for folder in ('by-path', 'by-id'):
        for path in sorted(Path('/dev/v4l', folder).glob('*')):
            if path.exists():
                aliases.setdefault(str(path.resolve()), []).append(str(path))
    for node, links in sorted(aliases.items()):
        try:
            properties = dict(line.split('=', 1) for line in command(
                'udevadm', 'info', '--query=property', '--name', node).splitlines() if '=' in line)
            if properties.get('ID_BUS') != 'usb':
                continue
            info = ctl(node, '--info')
            # Device Caps, not the union of capabilities on a multi-node device.
            caps = info.split('Device Caps', 1)[-1]
            if 'Video Capture' not in caps or 'Streaming' not in caps:
                continue
            serial = properties.get('ID_SERIAL_SHORT', '')
            stable = next((p for p in links if '/by-id/' in p), None) if serial else None
            stable = stable or next((p for p in links if '/by-path/' in p), links[0])
            name = re.search(r'Card type\s*:\s*(.+)', info)
            driver = re.search(r'Driver name\s*:\s*(.+)', info)
            card = name[1].strip() if name else properties.get('ID_MODEL', stable)
            halves = card.split(': ', 1)
            if len(halves) == 2 and halves[0] == halves[1]:
                card = halves[0]
            vendor = properties.get('ID_VENDOR', '').replace('_', ' ')
            label = f'{vendor} — {card}' if vendor and vendor.casefold() not in card.casefold() else card
            devices.append(dict(id=stable, path=stable, name=label,
                                driver=driver[1].strip() if driver else '',
                                vid=properties.get('ID_VENDOR_ID', ''), pid=properties.get('ID_MODEL_ID', ''),
                                available=True))
        except (RuntimeError, OSError, subprocess.TimeoutExpired):
            continue
    return sorted(devices, key=lambda d: d['id'])


def choose_device(devices, preferred, active):
    ids = sorted(d['id'] for d in devices if d['available'])
    if active in ids:
        return active  # No analog-signal based failover, nor stealing on replug.
    return preferred if preferred in ids else next(iter(ids), None)


class Store:
    def __init__(self, path):
        self.path = Path(path)
        self.data = {'preferred': None, 'devices': {}}
        if self.path.exists():
            value = json.loads(self.path.read_text())
            if not isinstance(value.get('devices'), dict):
                raise ValueError('Invalid video configuration')
            self.data.update(value)

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp = self.path.with_suffix('.tmp')
        with temp.open('w', encoding='utf-8') as out:
            json.dump(self.data, out, indent=2)
            out.flush()
            os.fsync(out.fileno())
        temp.replace(self.path)


class Capture:
    def __init__(self, store, gst, gobject):
        self.store, self.Gst, self.GObject = store, gst, gobject
        self.pipeline = None
        self.active = None
        self.devices = []
        self.detail = {}
        self.error = ''
        self.notice = ''
        self.jobs = queue.Queue(maxsize=16)
        self.results = {}
        self.snapshot = {}
        self.snapshot_lock = threading.Lock()
        self.stop_event = threading.Event()
        self.next_retry = 0
        self.encoder = 'openh264enc'  # Preserve the established sender's encoder.
        self.encoder_specs = self.encoder_properties()
        self.last_notice = 0
        self.generation = 0
        self.byte_sample = None
        self.current_bitrate = None

    def encoder_properties(self):
        element = self.Gst.ElementFactory.make(self.encoder)
        if element is None:
            raise RuntimeError('openh264enc is not installed')
        result = []
        for spec in element.list_properties():
            if not spec.flags & self.GObject.ParamFlags.WRITABLE or not spec.flags & self.GObject.ParamFlags.READABLE:
                continue
            value = element.get_property(spec.name)
            if spec.name in ('name', 'parent'):
                continue
            if hasattr(spec, 'enum_class'):
                menu = [{'value': int(k), 'label': v.value_nick}
                        for k, v in type(value).__enum_values__.items()]
                result.append(dict(name=spec.name, type='menu', menu=menu, value=int(value), default=int(spec.default_value)))
            elif isinstance(value, bool):
                result.append(dict(name=spec.name, type='bool', value=int(value), min=0, max=1, step=1,
                                   default=int(spec.default_value)))
            elif isinstance(value, int) and hasattr(spec, 'minimum'):
                result.append(dict(name=spec.name, type='int', value=value, min=spec.minimum,
                                   max=spec.maximum, step=1, default=spec.default_value))
        return result

    def inspect(self):
        if not self.active:
            self.detail = {}
            return
        dev = self.active
        formats = parse_formats(ctl(dev, '--list-formats-ext'))
        controls = parse_controls(ctl(dev, '--list-ctrls-menus'))
        inputs = choices(optional(dev, '--list-inputs'), r'Input\s*:\s*(\d+)\s*\n\s*Name\s*:\s*([^\n]+)')
        standards_text = optional(dev, '--list-standards')
        standards = []
        for block in re.split(r'Index\s*:', standards_text)[1:]:
            standard = re.search(r'ID\s*:\s*(0x[\da-fA-F]+)', block)
            name = re.search(r'Name\s*:\s*([^\n]+)', block)
            if standard and name:
                standards.append({'value': standard[1], 'label': name[1].strip()})
        self.detail = dict(formats=formats, controls=controls, inputs=inputs, standards=standards)
        self.read_actual()

    def read_actual(self):
        if not self.active:
            return
        actual = ctl(self.active, '--get-fmt-video') + optional(self.active, '--get-parm')
        pixel = re.search(r"Pixel Format\s*:\s*'([^']+)'", actual)
        size = re.search(r'Width/Height\s*:\s*(\d+)/(\d+)', actual)
        fps = re.search(r'Frames per second\s*:\s*([\d.]+)(?:\s*\((\d+)/(\d+)\))?', actual)
        self.detail['actual'] = dict(pixel=pixel[1] if pixel else None,
                                     width=int(size[1]) if size else None,
                                     height=int(size[2]) if size else None,
                                     fps=fps[1] if fps else None,
                                     fps_fraction=f'{fps[2]}/{fps[3]}' if fps and fps[2] else None,
                                     input=optional(self.active, '--get-input').strip(),
                                     standard=optional(self.active, '--get-standard').strip())
        # UVC format state can belong to the open file handle. A new v4l2-ctl
        # handle may report a different format from the streaming GStreamer FD.
        # Once streaming, negotiated source caps are the authoritative readback.
        if self.pipeline:
            caps = self.pipeline.get_by_name('capture').get_static_pad('src').get_current_caps()
            if caps and not caps.is_empty():
                structure = caps.get_structure(0)
                raw = {'YUY2': 'YUYV', 'I420': 'YU12', 'RGB': 'RGB3', 'BGR': 'BGR3', 'GRAY8': 'GREY'}
                native = structure.get_string('format')
                actual = self.detail['actual']
                if structure.get_name() == 'image/jpeg':
                    actual['pixel'] = 'MJPG'
                elif native:
                    actual['pixel'] = raw.get(native, native)
                for key in ('width', 'height'):
                    valid, value = structure.get_int(key)
                    if valid:
                        actual[key] = value
                valid, numerator, denominator = structure.get_fraction('framerate')
                if valid and denominator:
                    actual['fps'] = f'{numerator/denominator:.3f}'
                    actual['fps_fraction'] = f'{numerator}/{denominator}'
        self.detail['controls'] = parse_controls(ctl(self.active, '--list-ctrls-menus'))

    def stop_pipeline(self):
        if self.pipeline is not None:
            self.pipeline.set_state(self.Gst.State.NULL)
            result, _, _ = self.pipeline.get_state(3 * self.Gst.SECOND)
            if result == self.Gst.StateChangeReturn.ASYNC:
                # Do not construct a second pipeline while the first owns capture.
                raise RuntimeError('Capture pipeline did not stop within 3 seconds')
            self.pipeline = None

    def start_pipeline(self):
        self.stop_pipeline()
        if not self.active:
            return
        settings = self.store.data['devices'].get(self.active, {})
        # Do not take a device owned by another capture process.
        busy = subprocess.run(['fuser', str(Path(self.active).resolve())], capture_output=True, text=True, timeout=3)
        if busy.returncode != 1 or busy.stdout.strip() or busy.stderr.strip():
            raise RuntimeError('Capture device is busy or ownership could not be checked')
        if 'input' in settings:
            ctl(self.active, '--set-input=' + str(settings['input']))
        if 'standard' in settings:
            ctl(self.active, '--set-standard=' + str(settings['standard']))
        for name, value in settings.get('controls', {}).items():
            control = next((c for c in self.detail['controls'] if c['name'] == name), None)
            if control and control['writable']:
                ctl(self.active, f'--set-ctrl={name}={value}')
        mode = settings.get('format')
        if not mode:
            # Ask the device for its current native mode, not a hardcoded demo.
            self.read_actual()
            mode = self.detail['actual']
        pixel = mode.get('pixel')
        # GStreamer's own V4L2 mapping supports all formats it can negotiate.
        # Only constrain the requested format; don't mislabel unknown FOURCCs.
        raw = {'YUYV': 'YUY2', 'UYVY': 'UYVY', 'NV12': 'NV12', 'YU12': 'I420',
               'YV12': 'YV12', 'RGB3': 'RGB', 'BGR3': 'BGR', 'GREY': 'GRAY8'}
        if pixel in ('MJPG', 'JPEG'):
            caps = 'image/jpeg'
            decode = ' ! jpegdec'
        elif pixel in raw:
            caps = 'video/x-raw,format=' + raw[pixel]
            decode = ''
        else:
            raise ValueError(f'Unsupported capture FOURCC {pixel}; choose a raw or MJPEG mode')
        if mode.get('width') and mode.get('height'):
            caps += f",width={int(mode['width'])},height={int(mode['height'])}"
        if mode.get('fps'):
            rate = Fraction(str(mode.get('fps_fraction') or mode['fps'])).limit_denominator(1001)
            caps += f',framerate={rate.numerator}/{rate.denominator}'
        description = ('v4l2src name=capture io-mode=mmap do-timestamp=true ! ' + caps +
                       ' ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream' + decode +
                       ' ! videoconvert ! video/x-raw,format=I420 ! openh264enc name=encoder'
                       ' ! h264parse ! watchdog timeout=5000 ! rtph264pay pt=96 config-interval=1 mtu=1200'
                       ' ! udpsink name=transport host=192.168.144.3 port=5602 sync=false async=false')
        pipeline = self.Gst.parse_launch(description)
        pipeline.get_by_name('capture').set_property('device', self.active)
        encoder = pipeline.get_by_name('encoder')
        encoder.set_property('bitrate', 2000000)  # Existing stream configuration.
        encoder.set_property('complexity', 0)
        for name, value in settings.get('encoding', {}).items():
            encoder.set_property(name, value)
        self.pipeline = pipeline
        self.generation += 1
        self.byte_sample = None
        self.current_bitrate = None
        if pipeline.set_state(self.Gst.State.PLAYING) == self.Gst.StateChangeReturn.FAILURE:
            self.stop_pipeline()
            raise RuntimeError('GStreamer rejected capture configuration')
        result, _, _ = pipeline.get_state(3 * self.Gst.SECOND)
        if result == self.Gst.StateChangeReturn.FAILURE:
            bus_error = pipeline.get_bus().pop_filtered(self.Gst.MessageType.ERROR)
            message = bus_error.parse_error()[0].message if bus_error else 'Pipeline negotiation failed'
            self.stop_pipeline()
            raise RuntimeError(message)
        self.read_actual()
        self.error = ''

    def reconcile(self):
        found = discover()
        known = {d['id']: {**d, 'available': False} for d in self.devices}
        for device in found:
            known[device['id']] = device
        preferred = self.store.data.get('preferred')
        if preferred and preferred not in known:
            known[preferred] = dict(id=preferred, name=Path(preferred).name, available=False)
        self.devices = sorted(known.values(), key=lambda d: d['id'])
        selected = choose_device(self.devices, preferred, self.active)
        if selected != self.active:
            previous = self.active
            self.stop_pipeline()
            self.active = selected
            self.notice = f'Video source changed: {previous or "none"} → {selected or "none"}'
            self.last_notice = time.monotonic()
            self.inspect()
            if selected:
                self.start_pipeline()

    @staticmethod
    def validate_value(spec, value):
        if not isinstance(value, int) or isinstance(value, bool):
            raise ValueError('Control value must be an integer')
        if spec.get('menu'):
            if value not in [m['value'] for m in spec['menu']]:
                raise ValueError('Unsupported menu value')
        elif 'min' in spec and 'max' in spec:
            if not spec['min'] <= value <= spec['max'] or (value - spec['min']) % max(1, spec.get('step', 1)):
                raise ValueError('Value outside device range/step')
        elif spec['type'] != 'button':
            raise ValueError('Unsupported control type')

    def configure(self, action, payload):
        if action == 'refresh':
            self.reconcile()
            self.inspect()
            return
        if action == 'select':
            target = payload.get('device')
            if target not in [d['id'] for d in self.devices if d['available']]:
                raise ValueError('Capture device unavailable')
            self.stop_pipeline()
            self.active = target
            self.inspect()
            # A USB source remains selected even if analog video is absent or
            # encoder negotiation fails. Preference must survive that failure.
            self.store.data['preferred'] = target
            self.store.save()
            self.notice = 'Camera selected: ' + target
            self.last_notice = time.monotonic()
            self.start_pipeline()
            return
        if not self.active or payload.get('device') != self.active:
            raise ValueError('Active camera changed; refresh and try again')
        settings = self.store.data['devices'].setdefault(self.active, {})
        previous = copy.deepcopy(settings)
        if action in ('controls', 'reset'):
            self.read_actual()
            specs = {c['name']: c for c in self.detail['controls']}
            values = payload.get('values', {}) if action == 'controls' else {
                c['name']: c['default'] for c in specs.values()
                if c['writable'] and 'default' in c and c['type'] != 'button'}
            for name, value in values.items():
                spec = specs.get(name)
                if not spec or not spec['writable']:
                    raise ValueError('Control is unavailable or read-only: ' + name)
                self.validate_value(spec, value)
            try:
                for name, value in values.items():
                    ctl(self.active, f'--set-ctrl={name}={value}')
            except RuntimeError:
                # Some drivers require the stream stopped for particular controls.
                self.stop_pipeline()
                try:
                    for name, value in values.items():
                        ctl(self.active, f'--set-ctrl={name}={value}')
                finally:
                    self.read_actual()
                    settings.setdefault('controls', {}).update({c['name']: c['value'] for c in self.detail['controls']
                        if c['name'] in values and 'value' in c and c['type'] != 'button'})
                    self.start_pipeline()
            finally:
                self.read_actual()
                settings.setdefault('controls', {}).update({c['name']: c['value'] for c in self.detail['controls']
                    if c['name'] in values and 'value' in c and c['type'] != 'button'})
                self.store.save()
            return
        if action != 'configure':
            raise ValueError('Unknown video operation')
        settings = copy.deepcopy(previous)
        mode = payload.get('format')
        if mode:
            pixel = next((f for f in self.detail['formats'] if f['pixel'] == mode.get('pixel')), None)
            if not pixel:
                raise ValueError('Unsupported pixel format')
            width, height = int(mode['width']), int(mode['height'])
            def supported(size):
                r = size.get('range')
                return (width == size['width'] and height == size['height']) if not r else (
                    r['min_width'] <= width <= r['max_width'] and r['min_height'] <= height <= r['max_height'] and
                    (width-r['min_width']) % r['step_width'] == 0 and (height-r['min_height']) % r['step_height'] == 0)
            if not any(supported(s) for s in pixel['sizes']):
                raise ValueError('Unsupported resolution')
            rate = float(Fraction(str(mode['fps'])))
            if not math.isfinite(rate) or not 0 < rate <= 1000:
                raise ValueError('Invalid FPS')
            matching = next(s for s in pixel['sizes'] if supported(s))
            if matching['rates'] and not any(abs(rate-float(r)) < .001 for r in matching['rates']):
                raise ValueError('Unsupported FPS for this resolution')
            # Preserve NTSC rational rates when v4l2-ctl prints rounded decimals.
            fraction = Fraction(str(mode['fps'])).limit_denominator(1001)
            for denominator in (1001,):
                candidate = Fraction(round(rate * denominator / 1000) * 1000, denominator)
                if abs(float(candidate)-rate) < .0006:
                    fraction = candidate
            settings['format'] = dict(pixel=mode['pixel'], width=width, height=height, fps=str(fraction))
        for key, options in (('input', self.detail['inputs']), ('standard', self.detail['standards'])):
            if key in payload:
                if payload[key] not in [item['value'] for item in options]:
                    raise ValueError('Unsupported ' + key)
                settings[key] = payload[key]
        for name, value in payload.get('encoding', {}).items():
            spec = next((s for s in self.encoder_specs if s['name'] == name), None)
            if not spec:
                raise ValueError('Unknown encoder property')
            self.validate_value(spec, value)
            settings.setdefault('encoding', {})[name] = value
        try:
            self.store.data['devices'][self.active] = settings
            self.start_pipeline()
            # Persist negotiated values rather than the requested approximation.
            settings['format'] = {k: self.detail['actual'][k] for k in ('pixel', 'width', 'height', 'fps', 'fps_fraction')}
            self.store.save()
        except Exception:
            self.store.data['devices'][self.active] = previous
            self.start_pipeline()
            raise

    def publish(self):
        state = 'no devices' if not self.active else 'retrying'
        encoding = copy.deepcopy(self.encoder_specs)
        negotiated = None
        dropped = None
        if self.pipeline:
            _, current, _ = self.pipeline.get_state(0)
            state = current.value_nick
            encoder = self.pipeline.get_by_name('encoder')
            for spec in encoding:
                spec['value'] = int(encoder.get_property(spec['name']))
            pad = self.pipeline.get_by_name('capture').get_static_pad('src')
            caps = pad.get_current_caps()
            negotiated = caps.to_string() if caps else None
            transport = self.pipeline.get_by_name('transport')
            if transport.find_property('bytes-served'):
                total = transport.get_property('bytes-served')
                now = time.monotonic()
                if self.byte_sample and now-self.byte_sample[0] >= 1:
                    self.current_bitrate = round((total-self.byte_sample[1])*8/(now-self.byte_sample[0]))
                    self.byte_sample = (now, total)
                elif not self.byte_sample:
                    self.byte_sample = (now, total)
            if transport.find_property('stats'):
                stats = transport.get_property('stats')
                if stats and stats.has_field('dropped'):
                    dropped = int(stats.get_value('dropped'))
        snapshot = dict(devices=self.devices, active=self.active, preferred=self.store.data.get('preferred'),
                        detail=self.detail, encoder=self.encoder, encoding=encoding, negotiated=negotiated,
                        stream_state=state, error=self.error, pipeline_generation=self.generation,
                        bitrate_bps=self.current_bitrate if self.pipeline else None, sink_dropped_frames=dropped,
                        notice=self.notice if time.monotonic()-self.last_notice < 10 else '', operations=self.results)
        with self.snapshot_lock:
            self.snapshot = copy.deepcopy(snapshot)

    def run(self):
        next_scan = 0
        while not self.stop_event.is_set():
            try:
                now = time.monotonic()
                if now >= next_scan:
                    next_scan = now + 2
                    try:
                        self.reconcile()
                    except Exception as exc:
                        self.error = str(exc)
                        self.next_retry = now + 3
                if self.pipeline:
                    msg = self.pipeline.get_bus().pop_filtered(self.Gst.MessageType.ERROR | self.Gst.MessageType.EOS)
                    if msg:
                        if msg.type == self.Gst.MessageType.ERROR:
                            error, debug = msg.parse_error()
                            self.error = error.message
                            print(f'Video pipeline error: {error.message}; {debug}', flush=True)
                        else:
                            self.error = 'End of video stream'
                        self.stop_pipeline()
                        self.next_retry = now + 3
                if self.active and self.pipeline is None and now >= self.next_retry:
                    self.next_retry = now + 3
                    try:
                        self.inspect()
                        self.start_pipeline()
                    except Exception as exc:
                        self.error = str(exc)
                try:
                    identifier, action, payload = self.jobs.get_nowait()
                except queue.Empty:
                    pass
                else:
                    try:
                        self.configure(action, payload)
                        self.results[identifier] = {'done': True, 'error': ''}
                    except Exception as exc:
                        self.results[identifier] = {'done': True, 'error': str(exc)}
                        self.error = str(exc)
                    while len(self.results) > 32:
                        del self.results[next(iter(self.results))]
                self.publish()
            except Exception as exc:
                self.error = str(exc)
                self.next_retry = time.monotonic() + 3
                self.publish()
            self.stop_event.wait(.2)
        self.stop_pipeline()


def make_handler(capture, allowed_peer):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, data):
            body = json.dumps(data).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def authorized(self):
            # Dedicated trusted video link only. Browser origins are rejected.
            if self.client_address[0] not in (allowed_peer, '127.0.0.1', self.server.server_address[0]) or self.headers.get('Origin'):
                self.reply(403, {'error': 'Video management peer not allowed'})
                return False
            return True

        def do_GET(self):
            if not self.authorized():
                return
            if self.path != '/state':
                return self.reply(404, {'error': 'Unknown endpoint'})
            with capture.snapshot_lock:
                snapshot = copy.deepcopy(capture.snapshot)
            self.reply(200, snapshot)

        def do_POST(self):
            self.connection.settimeout(3)
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 65536:
                    raise ValueError('Invalid request size')
                body = self.rfile.read(length)
                if not self.authorized():
                    return
                if self.path not in ('/select', '/refresh', '/configure', '/controls', '/reset'):
                    return self.reply(404, {'error': 'Unknown endpoint'})
                payload = json.loads(body)
                if not isinstance(payload, dict):
                    raise ValueError('Expected JSON object')
                identifier = uuid.uuid4().hex
                capture.jobs.put_nowait((identifier, self.path[1:], payload))
                self.reply(202, {'operation': identifier})
            except queue.Full:
                self.reply(503, {'error': 'Video manager busy'})
            except (ValueError, OSError) as exc:
                self.reply(400, {'error': str(exc)})
    return Handler


def main():
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst, GObject
    Gst.init(None)
    store = Store(Path.home() / '.config/fpv-gcs/video-capture.json')
    capture = Capture(store, Gst, GObject)
    host = os.environ.get('FPV_VIDEO_API_BIND', '192.168.144.20')
    server = ThreadingHTTPServer((host, int(os.environ.get('FPV_VIDEO_API_PORT', '5603'))),
                                make_handler(capture, '192.168.144.3'))
    server.daemon_threads = True
    worker = threading.Thread(target=capture.run, name='video-owner')
    worker.start()
    def stop(*_):
        capture.stop_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        server.serve_forever(poll_interval=.2)
    finally:
        capture.stop_event.set()
        worker.join(timeout=6)
        server.server_close()


if __name__ == '__main__':
    main()
