#!/usr/bin/env python3
"""Actual encrypted WFB/FEC recovery and wrong-key rejection; development only.
Pinned wire format: packed wrxfwd_t=17 bytes; wblock_hdr_t is kind=1 plus
big-endian nonce=(block_idx<<8)|fragment_idx. k=2,n=3: 0/1=data,2=parity.
"""
import contextlib
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def udp():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', 0))
    return sock


def run(binary, work, drop):
    with contextlib.ExitStack() as stack:
        receiver, rejected, proxy, tx_res, rx_res, bad_res = [stack.enter_context(udp()) for _ in range(6)]
        txport, rxport, badport = [s.getsockname()[1] for s in (tx_res, rx_res, bad_res)]
        processes, logs, errors = [], [], []
        blocks, dropped, parity = set(), set(), set()
        stats = {'session': 0, 'bad_forwarded': 0}
        stop = threading.Event()

        def forward():
            proxy.settimeout(0.1)
            try:
                while not stop.is_set():
                    try:
                        packet = proxy.recv(65535)
                    except socket.timeout:
                        continue
                    assert len(packet) >= 18, 'Truncated forwarder frame'
                    # Wrong-key RX receives ALL traffic, including the dropped data.
                    proxy.sendto(packet, ('127.0.0.1', badport))
                    stats['bad_forwarded'] += 1
                    kind = packet[17]
                    if kind == 1:
                        assert len(packet) >= 26, 'Truncated data nonce'
                        nonce = struct.unpack_from('!Q', packet, 18)[0]
                        block, fragment = nonce >> 8, nonce & 255
                        blocks.add(block)
                        if fragment == 2:
                            parity.add(block)
                        if drop and fragment == 0:
                            dropped.add(block)
                            continue
                    elif kind == 2:
                        stats['session'] += 1
                    else:
                        raise AssertionError(f'Unexpected pinned WFB packet type {kind}')
                    proxy.sendto(packet, ('127.0.0.1', rxport))
            except Exception as exc:
                errors.append(exc)

        thread = threading.Thread(target=forward, daemon=True)

        def launch(name, args):
            log = stack.enter_context((work / f'{name}.log').open('wb'))
            logs.append((name, log))
            processes.append(subprocess.Popen(args, stdout=log, stderr=log))

        try:
            # CLI programs cannot inherit sockets; reserve until immediately before
            # each launch to minimize the unavoidable reservation handoff gap.
            rx_res.close()
            launch('rx', [str(binary/'wfb_rx'), '-a', str(rxport), '-K', str(work/'good/gs.key'),
                          '-c', '127.0.0.1', '-u', str(receiver.getsockname()[1]), '-p', '0'])
            bad_res.close()
            launch('rx-wrong', [str(binary/'wfb_rx'), '-a', str(badport), '-K', str(work/'wrong/gs.key'),
                                '-c', '127.0.0.1', '-u', str(rejected.getsockname()[1]), '-p', '0'])
            thread.start()
            tx_res.close()
            launch('tx', [str(binary/'wfb_tx'), '-D', str(proxy.getsockname()[1]), '-K', str(work/'good/drone.key'),
                          '-u', str(txport), '-p', '0', '-k', '2', '-n', '3', '-T', '100', 'dummy0'])
            time.sleep(0.4)
            assert all(p.poll() is None for p in processes), 'WFB exited at startup'
            payloads = [b'FPV-WFB-loopback:' + bytes([i])*1000 for i in range(8)]
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                for payload in payloads:
                    sender.sendto(payload, ('127.0.0.1', txport))
                    time.sleep(0.002)
            receiver.settimeout(4)
            assert [receiver.recv(4096) for _ in payloads] == payloads, 'Payload corruption/order error'
            rejected.settimeout(0.6)
            try:
                rejected.recv(65535)
            except socket.timeout:
                pass
            else:
                raise AssertionError('Wrong GS key produced plaintext')
            if errors:
                raise errors[0]
            assert len(blocks) == 4 and parity == blocks and stats['session'] > 0, 'Incomplete FEC/session observations'
            assert dropped == (blocks if drop else set()), 'Loss injection not exercised'
            assert stats['bad_forwarded'] >= 13, 'Wrong-key RX did not receive test traffic'
            assert all(p.poll() is None for p in processes), 'Receiver exited instead of rejecting wrong key'
            assert 'Unable to decrypt session key' in (work/'rx-wrong.log').read_text(errors='replace'), 'Wrong-key RX did not authenticate/reject the session'
            print(f'PASS: {"one data fragment dropped per block" if drop else "intact encrypted path"}; '
                  f'8 exact payloads, 4 FEC blocks, {len(dropped)} dropped; wrong GS key rejected output')
        except Exception:
            for name, log in logs:
                log.flush()
                print(f'--- {name} ---\n{(work/(name+".log")).read_text(errors="replace")}', file=sys.stderr)
            raise
        finally:
            stop.set()
            if thread.ident is not None:
                thread.join(timeout=2)
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='fpv-wfb-test-') as directory:
        work = pathlib.Path(directory)
        for name in ('good', 'wrong'):
            (work/name).mkdir()
            subprocess.run([str(binary/'wfb_keygen')], cwd=work/name, check=True)
        run(binary, work, drop=False)
        run(binary, work, drop=True)
