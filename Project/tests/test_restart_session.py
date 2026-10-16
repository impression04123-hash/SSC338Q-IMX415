#!/usr/bin/env python3
"""Air process restart must create a fresh RTP session accepted immediately."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
nal = b'\x26\x01\x80\x42'
annexb = b'\0\0\0\1' + nal
with tempfile.TemporaryDirectory(prefix='fpv restart ') as directory:
    root = Path(directory)
    source = root/'input.h265'
    source.write_bytes(annexb)
    config = root/'air.ini'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as capture:
        capture.bind(('127.0.0.1', 0))
        capture.settimeout(3)
        port = capture.getsockname()[1]
        config.write_text(f'[video]\nsource={source}\nfps=120\n[transport]\nport={port}\nssrc=123456\n')
        packets = []
        for _ in range(2):
            result = subprocess.run([str(binary/'fpv-air'), '--config', str(config)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
            assert result.returncode == 0, result.stderr.decode()
            packets.append(capture.recv(4096))
        assert packets[0][8:12] != packets[1][8:12], 'air reused SSRC after restart'
        config.write_text(config.read_text() + 'random_ssrc=0\n')
        subprocess.run([str(binary/'fpv-air'), '--config', str(config)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        fixed = capture.recv(4096)
        assert int.from_bytes(fixed[8:12], 'big') == 123456
    ground = root/'ground.ini'
    output = root/'received.h265'
    ground.write_text(f'[system]\nrole=ground\n[video]\noutput={output}\nmax_frames=2\n'
                      f'[transport]\nport={port}\n[runtime]\nidle_exit_ms=600\n')
    rx = subprocess.Popen([str(binary/'fpv-ground'), '--config', str(ground)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(.2)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            for packet, seq in zip(packets, (30000, 10000)):
                data = bytearray(packet)
                data[2:4] = seq.to_bytes(2, 'big')
                sender.sendto(data, ('127.0.0.1', port))
                time.sleep(.02)
        out, error = rx.communicate(timeout=3)
        assert rx.returncode == 0, error.decode()
        assert output.read_bytes() == annexb*2, 'receiver did not recover on restarted session'
    finally:
        if rx.poll() is None:
            rx.terminate()
            rx.communicate(timeout=3)
    print('PASS: fresh SSRC per air start, explicit fixed mode, immediate ground recovery on backward sequence')
