"""Video selection/persistence and actual v4l2-ctl output contracts; no hardware."""
import copy
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import queue
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

from video_capture import Capture, Store, choose_device, make_handler, parse_controls, parse_formats


class VideoTests(unittest.TestCase):
    def test_preferred_and_deterministic_first_device(self):
        devices = [dict(id='usb-b', available=True), dict(id='usb-a', available=True)]
        self.assertEqual(choose_device(devices, 'usb-b', None), 'usb-b')
        self.assertEqual(choose_device(devices, 'missing', None), 'usb-a')
        self.assertEqual(choose_device(devices, None, None), 'usb-a')

    def test_analog_loss_and_replug_do_not_steal_active_source(self):
        devices = [dict(id='preferred', available=True), dict(id='active', available=True)]
        self.assertEqual(choose_device(devices, 'preferred', 'active'), 'active')

    def test_physical_unplug_failover_and_late_attach(self):
        devices = [dict(id='old', available=False), dict(id='new', available=True)]
        self.assertEqual(choose_device(devices, 'old', 'old'), 'new')
        self.assertIsNone(choose_device([], 'old', None))
        self.assertEqual(choose_device(devices, 'old', None), 'new')

    def test_store_restores_independent_devices_atomically(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'video.json'
            store = Store(path)
            store.data = dict(preferred='a', devices={'a': {'controls': {'brightness': -12}}, 'b': {'encoding': {'bitrate': 3000000}}})
            store.save()
            self.assertEqual(Store(path).data, store.data)
            self.assertFalse(path.with_suffix('.tmp').exists())

    def test_malformed_store_is_not_silently_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'video.json'; path.write_text('{broken')
            with self.assertRaises(ValueError):
                Store(path)
            self.assertEqual(path.read_text(), '{broken')

    def test_v4l_controls_ranges_sparse_menu_and_flags(self):
        controls = parse_controls('''
 brightness 0x00980900 (int) : min=-128 max=127 step=1 default=0 value=-17 flags=has-min-max
 power_line_frequency 0x00980918 (menu) : min=0 max=2 default=1 value=1 (50 Hz)
     0: Disabled
     2: 60 Hz
 gain 0x00980913 (int) : min=0 max=65535 step=1 default=16 value=16 flags=inactive
 focus_status 0x00980914 (int) : min=0 max=1 step=1 default=0 value=1 flags=read-only
''')
        self.assertEqual(controls[0]['value'], -17)
        self.assertEqual(controls[0]['min'], -128)
        self.assertEqual([m['value'] for m in controls[1]['menu']], [0, 2])
        self.assertFalse(controls[2]['writable'])
        self.assertFalse(controls[3]['writable'])

    def test_v4l_format_rates_stay_with_their_resolution(self):
        formats = parse_formats('''
 [0]: 'YUYV' (YUYV 4:2:2)
   Size: Discrete 720x576
     Interval: Discrete 0.040s (25.000 fps)
   Size: Discrete 720x480
     Interval: Discrete 0.033s (30.000 fps)
 [1]: 'MJPG' (Motion-JPEG)
   Size: Discrete 640x480
     Interval: Discrete 0.017s (60.000 fps)
''')
        self.assertEqual(formats[0]['sizes'][0]['rates'], ['25.000'])
        self.assertEqual(formats[0]['sizes'][1]['rates'], ['30.000'])
        self.assertEqual(formats[1]['sizes'][0]['rates'], ['60.000'])

    def test_stepwise_modes_preserve_real_bounds_and_steps(self):
        formats = parse_formats('''[0]: 'YUYV'
 Size: Stepwise 32x32 - 1920x1080 with step 16/8
 Interval: Stepwise 0.020s - 0.200s with step 0.010s
''')
        size = formats[0]['sizes'][0]
        self.assertEqual(size['range']['step_height'], 8)
        self.assertEqual(size['interval_range']['min'], .02)

    def test_boolean_and_bitmask_controls_use_v4l_type_bounds(self):
        specs = parse_controls('''
 exposure_auto_priority 0x009a0903 (bool) : default=0 value=1
 led_mask 0x009a0904 (bitmask) : max=0x000f default=0x0001 value=0x0003
''')
        Capture.validate_value(specs[0], 0)
        Capture.validate_value(specs[1], 15)
        with self.assertRaises(ValueError): Capture.validate_value(specs[0], 2)
        with self.assertRaises(ValueError): Capture.validate_value(specs[1], 16)

    def test_validation_rejects_wrong_type_out_of_range_and_step(self):
        spec = dict(type='int', min=-10, max=10, step=2)
        for value in (True, '4', 11, -9):
            with self.assertRaises(ValueError): Capture.validate_value(spec, value)
        Capture.validate_value(spec, -8)
        with self.assertRaises(ValueError):
            Capture.validate_value(dict(type='menu', menu=[dict(value=0), dict(value=2)]), 1)

    def manager(self, folder):
        # Exercise transactional methods without importing GI on Windows.
        obj = Capture.__new__(Capture)
        obj.store = Store(Path(folder)/'video.json')
        obj.active = 'camera-a'
        obj.devices = [dict(id='camera-a', available=True), dict(id='camera-b', available=True)]
        obj.detail = dict(formats=[dict(pixel='MJPG', sizes=[dict(width=640, height=480, rates=['30.000'])])], inputs=[], standards=[], controls=[], actual=dict(pixel='MJPG', width=640, height=480, fps='30.000', fps_fraction='30/1'))
        obj.encoder_specs = [dict(name='bitrate', type='int', min=1, max=10000000, step=1)]
        obj.stop_pipeline = lambda: None
        obj.inspect = lambda: None
        obj.start_pipeline = lambda: None
        return obj

    def test_manual_preference_persists_even_when_video_signal_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            def fail(): raise RuntimeError('No frames')
            obj.start_pipeline = fail
            with self.assertRaises(RuntimeError): obj.configure('select', dict(device='camera-b'))
            self.assertEqual(Store(obj.store.path).data['preferred'], 'camera-b')

    def test_stale_camera_edit_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            with self.assertRaisesRegex(ValueError, 'Active camera changed'):
                obj.configure('controls', dict(device='camera-b', values=dict(brightness=1)))

    def test_invalid_encoding_cannot_leave_partial_capture_config(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            obj.store.data['devices']['camera-a'] = {'encoding': {'bitrate': 2000000}}
            previous = copy.deepcopy(obj.store.data)
            with self.assertRaises(ValueError):
                obj.configure('configure', dict(device='camera-a', format=dict(pixel='MJPG', width=640, height=480, fps='30'), encoding=dict(bitrate=-1)))
            self.assertEqual(obj.store.data, previous)

    def test_failed_pipeline_restores_previous_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            obj.store.data['devices']['camera-a'] = {'encoding': {'bitrate': 2000000}}
            attempts = []
            def start():
                attempts.append(copy.deepcopy(obj.store.data['devices']['camera-a']))
                if len(attempts) == 1: raise RuntimeError('Negotiation failed')
            obj.start_pipeline = start
            with self.assertRaises(RuntimeError):
                obj.configure('configure', dict(device='camera-a', encoding=dict(bitrate=3000000)))
            self.assertEqual(attempts[1]['encoding']['bitrate'], 2000000)

    def test_wrong_fps_is_rejected_before_pipeline_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            with self.assertRaisesRegex(ValueError, 'Unsupported FPS'):
                obj.configure('configure', dict(device='camera-a', format=dict(pixel='MJPG', width=640, height=480, fps='25')))

    def test_negotiated_caps_override_another_v4l_file_handles_format(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = self.manager(folder)
            structure = Mock()
            structure.get_name.return_value = 'video/x-raw'
            structure.get_string.return_value = 'YUY2'
            structure.get_int.side_effect = lambda key: (True, {'width': 720, 'height': 576}[key])
            structure.get_fraction.return_value = (True, 25, 1)
            caps = Mock(); caps.is_empty.return_value = False; caps.get_structure.return_value = structure
            obj.pipeline = Mock()
            obj.pipeline.get_by_name.return_value.get_static_pad.return_value.get_current_caps.return_value = caps
            def other_handle(device, *args):
                return "Width/Height : 640/480\nPixel Format : 'MJPG'\n" if '--get-fmt-video' in args else ''
            with patch('video_capture.ctl', side_effect=other_handle), patch('video_capture.optional', return_value=''):
                obj.read_actual()
            self.assertEqual(obj.detail['actual']['pixel'], 'YUYV')
            self.assertEqual(obj.detail['actual']['width'], 720)
            self.assertEqual(obj.detail['actual']['fps_fraction'], '25/1')

    def test_http_reads_snapshot_and_enqueues_without_touching_capture(self):
        class Fake:
            snapshot_lock = threading.Lock()
            snapshot = {'devices': []}
            jobs = queue.Queue(maxsize=1)
        fake = Fake()
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(fake, '192.168.144.3'))
        worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
        conn = HTTPConnection('127.0.0.1', server.server_port, timeout=2)
        try:
            conn.request('GET', '/state'); response = conn.getresponse()
            self.assertEqual(response.status, 200); self.assertEqual(json.loads(response.read()), {'devices': []})
            conn.request('POST', '/refresh', '{}', {'Content-Type': 'application/json'}); response = conn.getresponse()
            self.assertEqual(response.status, 202); identifier = json.loads(response.read())['operation']
            self.assertEqual(fake.jobs.get_nowait(), (identifier, 'refresh', {}))
            conn.request('POST', '/select', '{}', {'Origin': 'https://example.com'}); response = conn.getresponse()
            self.assertEqual(response.status, 403); response.read()
            conn.request('POST', '/configure', '[]'); response = conn.getresponse()
            self.assertEqual(response.status, 400); response.read()
        finally:
            conn.close(); server.shutdown(); server.server_close(); worker.join()


if __name__ == '__main__':
    unittest.main()
