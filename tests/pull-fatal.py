#!/usr/bin/env python3
"""Sequential typed-caller fatal exits must reap workers before caller callbacks."""
import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile

from images import LayerBarriers, Registry, owned_workers, published_roots, tar, wait_fds


def read_marker(fd, expected):
    assert select.select([fd], [], [], 5)[0], f'missing caller callback {expected!r}'
    got = os.read(fd, 1)
    assert got == expected, (expected, got)


def run(binary, env, kind):
    with tempfile.TemporaryDirectory() as work:
        root = pathlib.Path(work)
        marker, gate = root / 'marker', root / 'gate'
        os.mkfifo(marker)
        os.mkfifo(gate)
        markfd = os.open(marker, os.O_RDWR | os.O_NONBLOCK)
        gatefd = os.open(gate, os.O_RDWR | os.O_NONBLOCK)
        first = Registry()
        second = Registry([tar([('prior', b'completed')])])
        bad = tar([('../escape', b'bad')]) if kind == 'extract' else tar([('zero', b'zero')])
        failing = Registry([bad, tar([('one', b'one')])])
        gates = LayerBarriers(failing)
        if kind == 'transfer':
            failing.responses['/v2/fixture/blobs/' + gates.ids[1]] = (503, {}, b'failure')
        p = peer = None
        workers = {}
        try:
            with first, second, failing:
                p = subprocess.Popen([binary, str(root), first.ref, second.ref,
                                      'success' if kind == 'success' else failing.ref,
                                      str(marker), str(gate)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
                if kind != 'success':
                    assert gates.started[0].wait(5) and gates.started[1].wait(5), 'missing workers'
                    workers = owned_workers(p)
                    assert len(workers) == 2, workers
                    gates.release(1 if kind == 'transfer' else 0)
                    read_marker(markfd, b'b')
                    # Callback closes descriptors and blocks. This MUST be
                    # after every owned worker is killed AND reaped.
                    wait_fds(workers.values(), 3)
                    assert all(not pathlib.Path(f'/proc/{pid}').exists()
                               for pid in workers), 'worker not reaped before callback'
                    held = 0 if kind == 'transfer' else 1
                    assert gates.closed[held].wait(3), 'held socket survived callback'
                    assert len(published_roots(root)) == 2, 'failed image published'
                    # A competing pull of the same reference may start once
                    # the callback closes the lock, but not before workers die.
                    failing.request_hook = None
                    failing.responses = {}
                    fresh = Registry([tar([('retry', b'recovered')])])
                    failing.blobs, failing.manifest = fresh.blobs, fresh.manifest
                    peer = subprocess.Popen([str(pathlib.Path(__file__).resolve().parent.parent / 'gu-pull'),
                                             '--root', str(root), '--http-loopback', failing.ref],
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    out, err = peer.communicate(timeout=10)
                    assert peer.returncode == 0, (out, err)
                    assert any((image / 'retry').read_bytes() == b'recovered'
                               for image in published_roots(root) if (image / 'retry').exists())
                else:
                    read_marker(markfd, b'b')
                os.write(gatefd, b'x')
                read_marker(markfd, b'a')
                out, err = p.communicate(timeout=10)
                assert p.returncode == (0 if kind == 'success' else 1), (kind, p.returncode, out, err)
                assert out == b''
                if kind == 'transfer':
                    assert b'registry HTTP status 503' in err and b'layer worker failed' in err, err
                elif kind == 'extract':
                    assert b'geranium layer: unsafe tar path' in err, err
                else:
                    assert len(published_roots(root)) == 2
                print(f'pull fatal sequential {kind}: workers reaped before callback, retry fenced PASS', flush=True)
        finally:
            try:
                os.write(gatefd, b'x')
                gates.release_all()
            except (BrokenPipeError, OSError):
                pass
            for child in (p, peer):
                if child and child.poll() is None:
                    child.kill()
                    child.communicate(timeout=5)
            for fd in workers.values():
                os.close(fd)
            os.close(markfd)
            os.close(gatefd)
            gates.close()


if __name__ == '__main__':
    binary = sys.argv[1]
    env = dict(os.environ, GERANIUM_PULL_CPUS='2')
    for scenario in ('transfer', 'extract', 'success'):
        run(binary, env, scenario)
