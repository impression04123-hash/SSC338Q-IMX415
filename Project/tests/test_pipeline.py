#!/usr/bin/env python3
"""Real H.265 end-to-end test (FFmpeg), no camera/radio/display required."""
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def run(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE)


def stop_process(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def reserve_port(stack):
    sock = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    sock.bind(('127.0.0.1', 0))
    return sock


def check_transport(transport, root, bindir, wfbdir, ffmpeg, source, expected):
    with ExitStack() as stack:
        # Hold all reservations concurrently so the OS cannot select the same
        # port twice. Release each only immediately before its owner starts.
        output_socket = reserve_port(stack)
        input_socket = reserve_port(stack) if transport == 'wfb' else output_socket
        radio_socket = reserve_port(stack) if transport == 'wfb' else None
        output_port = output_socket.getsockname()[1]
        input_port = input_socket.getsockname()[1]
        radio_port = radio_socket.getsockname()[1] if radio_socket else None
        output = root / f'{transport}.h265'
        common = f'''[video]
backend=file
source={source}
output={output}
width=320
height=240
fps=30
[transport]
type={'udp' if transport == 'wfb' else transport}
host=127.0.0.1
bind_host=127.0.0.1
unix_name=fpv_test_{os.getpid()}_{transport}
mtu=400
[runtime]
idle_exit_ms=1200
'''
        air = root / f'{transport}-air.ini'
        ground = root / f'{transport}-ground.ini'
        # Keep role-specific ports in their transport section, without replacing
        # arbitrary text that could also appear in a file path.
        air.write_text('[system]\nrole=air\n' + common.replace('mtu=400\n', f'port={input_port}\nmtu=400\n'))
        ground.write_text('[system]\nrole=ground\n' + common.replace('mtu=400\n', f'port={output_port}\nmtu=400\n'))
        logs = []

        def launch(name, args):
            path = root / f'{transport}-{name}.log'
            log = stack.enter_context(path.open('w'))
            logs.append((path, log))
            process = subprocess.Popen(args, stdout=log, stderr=log)
            # Register immediately: any subsequent log/Popen/startup failure
            # still stops every successfully launched process before log closure.
            stack.callback(stop_process, process)
            return process

        try:
            wfb_processes = []
            if transport == 'wfb':
                subprocess.run([str(wfbdir / 'wfb_keygen')], cwd=root, check=True, timeout=5)
                radio_socket.close()
                wfb_processes.append(launch('wfb_rx', [str(wfbdir / 'wfb_rx'), '-a', str(radio_port),
                    '-K', str(root / 'gs.key'), '-c', '127.0.0.1', '-u', str(output_port), '-p', '0']))
                input_socket.close()
                wfb_processes.append(launch('wfb_tx', [str(wfbdir / 'wfb_tx'), '-D', str(radio_port),
                    '-K', str(root / 'drone.key'), '-u', str(input_port), '-p', '0',
                    '-k', '8', '-n', '12', '-T', '20', 'dummy0']))
                time.sleep(0.3)
                assert all(p.poll() is None for p in wfb_processes), 'WFB startup failed'
            output_socket.close()
            rx = launch('ground', [str(bindir / 'fpv-ground'), '--config', str(ground)])
            time.sleep(0.2)
            assert rx.poll() is None, 'Ground receiver exited at startup'
            subprocess.run([str(bindir / 'fpv-air'), '--config', str(air)], check=True, timeout=10)
            assert rx.wait(timeout=5) == 0, 'Ground receiver failed'
            assert all(p.poll() is None for p in wfb_processes), 'WFB exited during transmission'
            actual = run(ffmpeg, '-v', 'error', '-i', str(output), '-f', 'framemd5', '-')
            assert expected == actual, f'{transport}: decoded frame hashes differ'
            assert len([line for line in actual.splitlines() if not line.startswith(b'#')]) == 35
            print(f'PASS {transport}: 35 decoded frames, identical frame MD5; '
                  f'input={source.stat().st_size} output={output.stat().st_size}')
        except Exception:
            for path, log in logs:
                log.flush()
                print(f'--- {path.name} ---\n{path.read_text(errors="replace")}', file=sys.stderr)
            raise


def main():
    bindir = Path(sys.argv[1]).resolve()
    wfbdir = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('SKIP: ffmpeg not installed')
        return 77
    with tempfile.TemporaryDirectory(prefix='fpv pipeline ') as tmp:
        root = Path(tmp)
        source = root / 'input.h265'
        run(ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=30',
            '-frames:v', '35', '-c:v', 'libx265', '-preset', 'ultrafast',
            '-x265-params', 'pools=1:frame-threads=1:bframes=0:keyint=15:repeat-headers=1:log-level=error',
            '-f', 'hevc', str(source))
        expected = run(ffmpeg, '-v', 'error', '-i', str(source), '-f', 'framemd5', '-')
        for transport in (('udp', 'unix', 'wfb') if wfbdir else ('udp', 'unix')):
            check_transport(transport, root, bindir, wfbdir, ffmpeg, source, expected)
    return 0


if __name__ == '__main__':
    sys.exit(main())
