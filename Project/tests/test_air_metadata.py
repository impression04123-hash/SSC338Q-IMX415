#!/usr/bin/env python3
"""Host-only file replay checks on real UDP RTP packets, without a decoder.

Fixtures use HEVC NAL headers and first-slice flags, with deliberately tiny
payloads. They test framing metadata, not decoder conformance; test_pipeline.py
separately verifies decoded pictures from a real x265 bitstream.
"""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build-host/fpv-air').resolve()


def nal(kind, payload=b'\x55', layer=0):
    return bytes([(kind << 1) | (layer >> 5), ((layer & 31) << 3) | 1]) + payload


class AirMetadataTests(unittest.TestCase):
    def transmit(self, units, loop=0, max_frames=0):
        with tempfile.TemporaryDirectory(prefix='fpv metadata ') as tmp:
            root = Path(tmp)
            source = root / 'input.h265'
            source.write_bytes(b''.join(b'\x00\x00\x00\x01' + unit for unit in units))
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
                receiver.bind(('127.0.0.1', 0))
                receiver.settimeout(.05)
                cfg = root / 'air.ini'
                cfg.write_text(f'''[system]
role=air
[video]
backend=file
source={source}
fps=30
loop={loop}
max_frames={max_frames}
[transport]
type=udp
host=127.0.0.1
port={receiver.getsockname()[1]}
payload_type=96
ssrc=0x11223344
random_ssrc=0
mtu=1200
''')
                result = subprocess.run([str(BINARY), '--config', str(cfg)],
                                        capture_output=True, text=True, timeout=5)
                packets = []
                while True:
                    try:
                        packets.append(receiver.recv(4096))
                    except socket.timeout:
                        break
                metadata = []
                previous = None
                for packet in packets:
                    self.assertGreaterEqual(len(packet), 14)
                    self.assertEqual(packet[0], 0x80)
                    self.assertEqual(packet[1] & 127, 96)
                    sequence, timestamp, ssrc = struct.unpack('!HII', packet[2:12])
                    self.assertEqual(ssrc, 0x11223344)
                    if previous is not None:
                        self.assertEqual(sequence, (previous + 1) & 0xffff)
                    previous = sequence
                    metadata.append((packet[12:], timestamp, bool(packet[1] & 128)))
                return result, metadata

    def assert_transmission(self, units, expected, **options):
        result, actual = self.transmit(units, **options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(actual, expected)

    def test_reserved_prefix_belongs_to_following_access_unit(self):
        # Catches assigning prefix NALs to the preceding picture timestamp.
        first, second = nal(1, b'\x80\x55'), nal(1, b'\x80\x66')
        for kind in (41, 42, 44):
            with self.subTest(prefix=kind):
                prefix = nal(kind)
                self.assert_transmission([first, prefix, second], [
                    (first, 0, True), (prefix, 3000, False), (second, 3000, True)])

    def test_multiple_slices_and_suffix_sei_share_access_unit(self):
        # Catches marking each slice as a whole frame or moving suffix SEI.
        first, continuation = nal(1, b'\x80\x55'), nal(1, b'\x40\x66')
        suffix, second = nal(40), nal(1, b'\x80\x77')
        self.assert_transmission([first, continuation, suffix, second], [
            (first, 0, False), (continuation, 0, False), (suffix, 0, True),
            (second, 3000, True)])

    def test_parameter_sets_and_prefix_sei_share_following_picture(self):
        vps, sps, pps, prefix = nal(32), nal(33), nal(34), nal(39)
        first, second = nal(1, b'\x80\x55'), nal(1, b'\x80\x66')
        units = [vps, sps, pps, first, vps, sps, pps, prefix, second]
        self.assert_transmission(units, [
            (vps, 0, False), (sps, 0, False), (pps, 0, False), (first, 0, True),
            (vps, 3000, False), (sps, 3000, False), (pps, 3000, False),
            (prefix, 3000, False), (second, 3000, True)])

    def test_aud_starts_following_access_unit(self):
        aud, first, second = nal(35), nal(1, b'\x80\x55'), nal(1, b'\x80\x66')
        self.assert_transmission([aud, first, aud, second], [
            (aud, 0, False), (first, 0, True), (aud, 3000, False), (second, 3000, True)])

    def test_nonzero_layer_is_rejected_before_transmission(self):
        # Catches silently treating each enhancement layer as another frame.
        for layer in (1, 32, 63):
            with self.subTest(layer=layer):
                result, packets = self.transmit([nal(1, b'\x80\x55', layer=layer)])
                self.assertNotEqual(result.returncode, 0, 'file replay must reject unsupported layers')
                self.assertEqual(packets, [], 'unsupported layer was transmitted')

    def test_loop_preserves_timestamp_progression_and_max_frames(self):
        # Catches resetting timestamps on rewind or replaying beyond the limit.
        vps, sps, pps = nal(32), nal(33), nal(34)
        first, second = nal(1, b'\x80\x55'), nal(1, b'\x80\x66')
        self.assert_transmission([vps, sps, pps, first, second], [
            (vps, 0, False), (sps, 0, False), (pps, 0, False), (first, 0, True),
            (second, 3000, True),
            (vps, 6000, False), (sps, 6000, False), (pps, 6000, False), (first, 6000, True),
            (second, 9000, True),
            (vps, 12000, False), (sps, 12000, False), (pps, 12000, False), (first, 12000, True),
        ], loop=1, max_frames=5)

    def test_max_frames_stops_at_picture_boundary_with_multiple_slices(self):
        first, continuation, second = nal(1, b'\x80\x55'), nal(1, b'\x40\x66'), nal(1, b'\x80\x77')
        self.assert_transmission([first, continuation, second], [
            (first, 0, False), (continuation, 0, True)], max_frames=1)


if __name__ == '__main__':
    unittest.main()
