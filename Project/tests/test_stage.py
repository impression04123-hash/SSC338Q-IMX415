#!/usr/bin/env python3
"""Validate actual CMake/stage output layout, licenses and destination guard."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
wfb = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
with tempfile.TemporaryDirectory(prefix='fpv staging ') as directory:
    stage = Path(directory) / 'stage'
    command = ['bash', str(root/'scripts/stage_target.sh'), str(build), str(stage)]
    if wfb:
        command.append(str(wfb))
    subprocess.run(command, check=True, stdout=subprocess.PIPE)
    for name in ('fpv-air', 'fpv-ground', 'fpvctl'):
        assert (stage/'usr/bin'/name).read_bytes() == (build/name).read_bytes()
    for name in ('air.ini', 'ground.ini', 'demo-air.ini', 'demo-ground.ini'):
        assert (stage/'etc/fpv'/name).read_bytes() == (root/'configs'/name).read_bytes()
    assert not (stage/'usr/etc/fpv').exists(), 'configuration installed under wrong prefix'
    assert not list(stage.rglob('*.key')), 'private key must not be part of stage'
    assert (stage/'usr/share/fpv/S95fpv').is_file()
    assert (stage/'usr/share/fpv/LICENSE').read_bytes() == (root/'LICENSE').read_bytes()
    notices = [('OpenIPC-research/LICENSE', 'third_party/sdk/RESEARCH-LICENSE'),
               ('msposd/LICENSE', 'third_party/msposd/LICENSE')]
    if wfb:
        notices.append(('wfb-ng/LICENSE.txt', 'third_party/wfb-ng/LICENSE.txt'))
    for target, source in notices:
        assert (stage/'usr/share/fpv/licenses'/target).read_bytes() == (root/source).read_bytes()
    original = (stage/'etc/fpv/air.ini').read_bytes()
    assert subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode != 0
    assert (stage/'etc/fpv/air.ini').read_bytes() == original
    print('PASS stage: real binaries/config/layout/notices, no keys, existing destination preserved')
