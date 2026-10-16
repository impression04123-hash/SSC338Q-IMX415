#!/usr/bin/env python3
"""Host-only process integration tests. Target fpvctl requires no Python."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build/host/fpvctl').resolve()

class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(BINARY.is_file(), 'fpvctl implementation is missing')
        self.tmp = tempfile.TemporaryDirectory(prefix='fpv argv ; space ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.cfg = self.root / 'role ; $(touch INJECTED).ini'
        self.source = self.root / 'sample.h265'
        self.source.write_bytes(b'\0\0\0\1\x40\x01')
        self.processes = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for p in self.processes:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            if p.stdout:
                p.stdout.close()
            if p.stderr:
                p.stderr.close()

    def config(self, extra='', wireless=False, role='air', **runtime):
        values = dict(restart_limit=2, restart_delay_ms=40)
        values.update(runtime)
        self.cfg.write_text(f'''[system]
role={role}
[video]
backend=file
source={self.source}
output={self.root / 'out.h265'}
[transport]
type=udp
host=127.0.0.1
bind_host=127.0.0.1
port=5602
[wireless]
enabled={int(wireless)}
iface=lo
driver=88XXeu
channel=161
bandwidth=20
mcs=2
fec_k=8
fec_n=12
radio_port=7
link_id=7669206
key_path={self.root / 'key ; $x'}
[osd]
enabled=0
[runtime]
bin_dir={self.bin}
run_dir={self.root / 'run'}
log_dir={self.root / 'logs'}
''' + ''.join(f'{k}={v}\n' for k, v in values.items()) + extra)

    def fake(self, name, mode='wait'):
        path = self.bin / name
        path.write_text('''#!/bin/sh
printf '%s\\n' "$$" >> "${0}.pids"
printf '%s\\n' "$@" > "${0}.args"
''' + ('''exit 9
''' if mode == 'crash' else ('''printf 'FPV_READY\\n' >&2
''' if mode == 'ready' else '') + '''sleep 1000 &
child=$!
printf '%s\\n' "$child" > "${0}.descendant"
trap 'wait "$child"; exit 0' TERM INT
wait "$child"
'''))
        path.chmod(0o755)

    def start(self):
        p = subprocess.Popen([str(BINARY), 'run', '--config', str(self.cfg)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(p)
        return p

    def wait_file(self, path):
        self.wait_for(path.exists, f'child did not start: {path}')

    def wait_for(self, predicate, message):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.02)
        self.fail(message)

    def python_child(self, name, body):
        path = self.bin / name
        path.write_text(f'#!{sys.executable}\n' + '''import os, signal, sys, time
from pathlib import Path
base = Path(__file__).parent
name = Path(__file__).name
pids = base / (name + '.pids')
count = len(pids.read_text().splitlines()) + 1 if pids.exists() else 1
with pids.open('a') as f:
    f.write(str(os.getpid()) + '\\n')
def event(value):
    with (base / 'events').open('a') as f:
        f.write(value + '\\n')
def gate(value):
    while not (base / value).exists():
        time.sleep(.01)
''' + body)
        path.chmod(0o755)

    def events(self):
        path = self.bin / 'events'
        return path.read_text().splitlines() if path.exists() else []

    def test_dry_run_exact_argv_and_no_side_effects(self):
        # Catches shell interpolation, split paths, swapped WFB radio/UDP ports.
        self.config(wireless=True)
        result = subprocess.run([str(BINARY), 'dry-run', '--config', str(self.cfg)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(s) for s in result.stdout.splitlines() if s.startswith('[')]
        tx = next(a for a in commands if a[0].endswith('/wfb_tx'))
        self.assertEqual(tx[tx.index('-u') + 1], '5602')
        self.assertEqual(tx[tx.index('-p') + 1], '7')
        self.assertEqual(tx[tx.index('-K') + 1], str(self.root / 'key ; $x'))
        air = next(a for a in commands if a[0].endswith('/fpv-air'))
        self.assertEqual(air, [str(self.bin / 'fpv-air'), '--config', str(self.cfg)])
        self.assertFalse(any('txpower' in a for a in commands), 'unset transmit power must retain the driver setting')
        self.assertFalse((self.root / 'run').exists())
        self.assertFalse((self.root / 'logs').exists())
        self.assertFalse((self.root / 'INJECTED').exists())

    def test_abstract_unix_uses_wfb_U_and_ground_starts_before_rx(self):
        self.config(wireless=True)
        text = self.cfg.read_text().replace('type=udp', 'type=unix\nunix_name=fpv-test sock')
        self.cfg.write_text(text)
        result = subprocess.run([str(BINARY), 'dry-run', '--config', str(self.cfg)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(s) for s in result.stdout.splitlines() if s.startswith('[')]
        tx = next(a for a in commands if a[0].endswith('/wfb_tx'))
        self.assertEqual(tx[tx.index('-U') + 1], 'fpv-test sock')
        self.assertNotIn('-u', tx)
        self.config(wireless=True, role='ground')
        result = subprocess.run([str(BINARY), 'dry-run', '--config', str(self.cfg)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(s) for s in result.stdout.splitlines() if s.startswith('[')]
        names = [Path(a[0]).name for a in commands]
        self.assertLess(names.index('fpv-ground'), names.index('wfb_rx'))

    def test_failed_child_has_finite_restart_budget(self):
        # Catches restart-forever and off-by-one restart limits.
        self.config()
        self.fake('fpv-air', 'crash')
        p = self.start()
        _, error = p.communicate(timeout=6)
        self.assertNotEqual(p.returncode, 0, error)
        starts = (self.bin / 'fpv-air.pids').read_text().splitlines()
        self.assertEqual(len(starts), 3)

    def osd_config(self, width, height, hardware=False, **runtime):
        self.config(**runtime)
        self.cfg.write_text(self.cfg.read_text()
                            .replace('backend=file\n', f'backend={"sigmastar" if hardware else "file"}\nwidth={width}\nheight={height}\n')
                            .replace('[osd]\nenabled=0', '[osd]\nenabled=1\nserial_device=/dev/null\nserial_baud=115200\nfps=20'))

    def test_osd_preview_uses_video_dimensions(self):
        # Catches msposd falling back to a stale majestic.yaml resolution.
        for width, height in ((1920, 1080), (1280, 720)):
            with self.subTest(width=width, height=height):
                self.osd_config(width, height)
                result = subprocess.run([str(BINARY), 'dry-run', '--config', str(self.cfg)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                commands = [json.loads(s) for s in result.stdout.splitlines() if s.startswith('[')]
                osd = next(a for a in commands if a[0].endswith('/msposd'))
                self.assertEqual(osd, [str(self.bin / 'msposd'), '--master', '/dev/null',
                                       '--baudrate', '115200', '--size', f'{width}x{height}', '--osd', '-r', '20'])
                self.assertFalse((self.root / 'run').exists())

    def test_osd_execution_receives_explicit_video_size(self):
        self.osd_config(1280, 720)
        self.fake('fpv-air', 'ready')
        self.fake('msposd')
        p = self.start()
        args = self.bin / 'msposd.args'
        self.wait_file(args)
        self.assertEqual(args.read_text().splitlines(), ['--master', '/dev/null',
                         '--baudrate', '115200', '--size', '1280x720', '--osd', '-r', '20'])
        p.terminate()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)

    def test_osd_waits_for_complete_exact_video_ready_line(self):
        # Catches spawn-as-readiness, substring matching and lost pipe fragments.
        self.osd_config(1280, 720, hardware=True)
        self.python_child('fpv-air', '''os.write(2, b'prefix FPV_READY\\nFPV_READY suffix\\nFPV_READY\\r\\n')
os.write(2, b'x' * 70000 + b'FPV_READY\\ninvalid-lines-done\\n')
gate('send-prefix')
os.write(2, b'FPV_')
gate('send-suffix')
os.write(2, b'READY')
gate('send-newline')
os.write(2, b'\\n')
while True:
    time.sleep(1)
''')
        self.fake('msposd')
        p = self.start()
        log = self.root / 'logs' / 'air-fpv-air.log'
        self.wait_for(lambda: log.exists() and b'invalid-lines-done\n' in log.read_bytes(),
                      'video log was not drained')
        starts = self.bin / 'msposd.pids'
        self.assertFalse(starts.exists(), 'OSD started on an invalid READY line')
        (self.bin / 'send-prefix').touch()
        self.wait_for(lambda: log.read_bytes().endswith(b'FPV_'), 'READY prefix was not logged')
        self.assertFalse(starts.exists(), 'OSD started on an incomplete prefix')
        (self.bin / 'send-suffix').touch()
        self.wait_for(lambda: log.read_bytes().endswith(b'FPV_READY'), 'READY suffix was not logged')
        self.assertFalse(starts.exists(), 'OSD started before the line terminator')
        (self.bin / 'send-newline').touch()
        self.wait_file(starts)
        self.assertTrue(log.read_bytes().endswith(b'FPV_READY\n'))
        p.terminate()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)

    def test_video_restart_recreates_osd_after_new_video_readiness(self):
        # Catches stale regions, reused READY state and dependent budget charges.
        self.osd_config(1280, 720, hardware=True, restart_limit=1)
        self.python_child('fpv-air', '''event('air%d-start' % count)
if count == 2:
    gate('air2-ready')
event('air%d-ready' % count)
os.write(2, b'FPV_READY\\n')
if count == 1:
    gate('air1-crash')
    sys.exit(9)
while True:
    time.sleep(1)
''')
        self.python_child('msposd', '''def stopped(sig, frame):
    time.sleep(.08)
    event('osd%d-stopped' % count)
    sys.exit(0)
signal.signal(signal.SIGTERM, stopped)
event('osd%d-start' % count)
if count == 2:
    gate('osd2-crash')
    sys.exit(9)
while True:
    time.sleep(1)
''')
        p = self.start()
        self.wait_for(lambda: 'osd1-start' in self.events(), 'initial OSD did not start')
        (self.bin / 'air1-crash').touch()
        self.wait_for(lambda: 'air2-start' in self.events(), 'video was not restarted')
        events = self.events()
        self.assertIn('osd1-stopped', events, 'old OSD survived into the new video pipeline')
        self.assertLess(events.index('osd1-stopped'), events.index('air2-start'))
        self.assertNotIn('osd2-start', events, 'OSD restarted before new video readiness')
        (self.bin / 'air2-ready').touch()
        self.wait_for(lambda: 'osd2-start' in self.events(), 'dependent OSD was not restarted')
        events = self.events()
        self.assertLess(events.index('air2-ready'), events.index('osd2-start'))
        (self.bin / 'osd2-crash').touch()
        self.wait_for(lambda: 'osd3-start' in self.events(),
                      'dependency restart consumed the OSD failure budget')
        self.assertIsNone(p.poll())
        p.terminate()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)

    def test_osd_own_failures_have_finite_restart_budget(self):
        self.osd_config(1280, 720, hardware=True, restart_limit=1)
        self.fake('fpv-air', 'ready')
        self.fake('msposd', 'crash')
        p = self.start()
        p.communicate(timeout=6)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(len((self.bin / 'msposd.pids').read_text().splitlines()), 2)

    def test_clean_file_end_before_osd_readiness_is_success(self):
        self.osd_config(1280, 720)
        self.fake('fpv-air', 'crash')
        path = self.bin / 'fpv-air'
        path.write_text(path.read_text().replace('exit 9', 'exit 0'))
        self.fake('msposd')
        p = self.start()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(len((self.bin / 'fpv-air.pids').read_text().splitlines()), 1)
        self.assertFalse((self.bin / 'msposd.pids').exists())

    @unittest.skipUnless(os.geteuid() == 0, 'wireless preflight requires root; fake tools never touch hardware')
    def test_crash_before_handoff_readiness_has_finite_retries(self):
        # Catches bypassing supervision when wfb_tx crashes during startup.
        self.config(wireless=True)
        (self.root / 'key ; $x').write_bytes(bytes(64))
        for name in ('modprobe', 'ip', 'iw'):
            path = self.bin / name
            path.write_text('#!/bin/sh\nexit 0\n')
            path.chmod(0o755)
        self.fake('wfb_tx', 'crash')
        self.fake('fpv-air')
        p = self.start()
        p.communicate(timeout=8)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(len((self.bin / 'wfb_tx.pids').read_text().splitlines()), 3)
        self.assertFalse((self.bin / 'fpv-air.pids').exists())

    def test_signal_cleans_group_and_unrelated_process_survives(self):
        # Catches global killall, cleanup limited to direct PID, and stale lock.
        self.config()
        self.fake('fpv-air')
        unrelated = subprocess.Popen(['sleep', '1000'])
        self.processes.append(unrelated)
        p = self.start()
        desc = self.bin / 'fpv-air.descendant'
        self.wait_file(desc)
        descendant = int(desc.read_text())
        p.send_signal(signal.SIGTERM)
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertIsNone(unrelated.poll())
        with self.assertRaises(ProcessLookupError):
            os.kill(descendant, 0)
        p2 = self.start()
        time.sleep(.15)
        self.assertIsNone(p2.poll())
        p2.terminate()
        p2.communicate(timeout=5)

    def test_single_instance_rejected_without_stopping_owner(self):
        self.config()
        self.fake('fpv-air')
        p = self.start()
        self.wait_file(self.bin / 'fpv-air.descendant')
        other = self.start()
        other.communicate(timeout=3)
        self.assertNotEqual(other.returncode, 0)
        self.assertIsNone(p.poll())

    def test_clean_file_end_is_success_without_restarts(self):
        self.config()
        self.fake('fpv-air', 'crash')
        path = self.bin / 'fpv-air'
        path.write_text(path.read_text().replace('exit 9', 'exit 0'))
        p = self.start()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(len((self.bin / 'fpv-air.pids').read_text().splitlines()), 1)

if __name__ == '__main__':
    unittest.main()
