#!/usr/bin/env python3
"""Observable status from real binaries; stdout remains command/workload data."""
import os
import pathlib
import select
import shutil
import subprocess
import tempfile
import threading
import time
from images import Registry, OCI, tar, call

BASE = pathlib.Path(__file__).resolve().parent.parent


def lines(p):
    assert p.stdout == b'', p.stdout
    assert p.stderr.endswith(b'\n') and b'\r' not in p.stderr and b'\x1b' not in p.stderr, p.stderr
    assert all(len(line) < 160 for line in p.stderr.splitlines()), p.stderr
    return p.stderr.decode().splitlines()


def pull_progress():
    with tempfile.TemporaryDirectory() as w, Registry(layers=[tar([('first', b'a')]), tar([('second', b'b')])]) as r:
        p = call('pull', '--root', w, '--http-loopback', r.ref)
        progress = lines(p)
        assert progress[:2] == ['geranium: fetching manifest', 'geranium: fetching config'], p.stderr
        assert progress[-1] == 'geranium: pulled', p.stderr
        expected = [f'geranium: layer {i}/2 {phase}' for i in (1, 2)
                    for phase in ('downloading', 'verifying', 'extracting')]
        assert sorted(progress[2:-1]) == sorted(expected), p.stderr
        for i in (1, 2):
            positions = [progress.index(f'geranium: layer {i}/2 {phase}')
                         for phase in ('downloading', 'verifying', 'extracting')]
            assert positions == sorted(positions), p.stderr
        assert progress.index('geranium: layer 1/2 extracting') < progress.index('geranium: layer 2/2 extracting'), p.stderr
        assert len(list(pathlib.Path(w).glob('.images/*/rootfs'))) == 1
        assert lines(call('pull', '--root', w, '--http-loopback', r.ref)) == ['geranium: already present']
        assert lines(call('rmi', '--root', w, r.ref)) == ['geranium: removed image']
        r.responses['/v2/fixture/manifests/latest'] = (200, {'Content-Type': OCI+'manifest.v1+json'}, b'{}')
        bad = call('pull', '--root', w, '--http-loopback', r.ref, ok=False)
        assert b'pulled' not in bad.stderr and b'already present' not in bad.stderr, bad.stderr
        assert not [p for p in pathlib.Path(w).glob('.images/*/rootfs') if not p.parent.name.startswith('.')]
    print('output pull fresh/cache/rejected PASS')


def bounded_line(stream, timeout):
    deadline = time.monotonic() + timeout
    line = bytearray()
    fd = stream.fileno()
    blocking = os.get_blocking(fd)
    os.set_blocking(fd, False)
    try:
        while not line.endswith(b'\n'):
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([fd], [], [], remaining)[0], 'complete status line timed out'
            byte = os.read(fd, 1)
            assert byte and len(line) < 160, 'missing/oversized status line'
            line += byte
        return bytes(line)
    finally:
        os.set_blocking(fd, blocking)


def pull_live():
    entered, release = threading.Event(), threading.Event()

    class HeldRegistry(Registry):
        def route(self, path, headers):
            if '/manifests/' in path and not release.is_set():
                entered.set()
                assert release.wait(15), 'origin release timeout'
            return super().route(path, headers)

    with tempfile.TemporaryDirectory() as w, HeldRegistry() as r:
        p = subprocess.Popen([str(BASE/'gu-pull'), '--root', w, '--http-loopback', r.ref], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert p.stderr is not None
            assert entered.wait(8), 'manifest request not received'
            assert bounded_line(p.stderr, 2) == b'geranium: fetching manifest\n'
            assert p.poll() is None, 'pull completed despite held response'
            release.set()
            stdout, stderr = p.communicate(timeout=8)
            assert p.returncode == 0 and stdout == b'' and stderr.endswith(b'geranium: pulled\n'), (p.returncode, stdout, stderr)
        finally:
            release.set()
            if p.poll() is None:
                p.kill()
            p.communicate(timeout=5)
    print('output held-origin live visibility PASS')


def mutation():
    with tempfile.TemporaryDirectory() as w:
        source = pathlib.Path(w)/'source'
        source.mkdir()
        (source/'marker').write_text('hi')
        root = pathlib.Path(w)/'state'
        assert lines(call('create', '--root', root, '--rootfs', source, 'demo', '/bin/true')) == ['geranium: created']
        inspect = call('inspect', '--root', root, 'demo')
        assert inspect.stdout == b'name=demo\nstatus=created\n' and inspect.stderr == b'', (inspect.stdout, inspect.stderr)
        assert lines(call('rm', '--root', root, 'demo')) == ['geranium: removed container']
        bad = call('create', '--root', root, '--rootfs', source, 'bad!name', '/bin/true', ok=False)
        assert b'created' not in bad.stderr
    print('output create/inspect/rm PASS')


def runtime():
    with tempfile.TemporaryDirectory() as w:
        source = pathlib.Path(w)/'source'
        (source/'bin').mkdir(parents=True)
        shutil.copyfile(os.environ.get('GERANIUM_TEST_BUSYBOX', shutil.which('busybox')), source/'bin/busybox')
        (source/'bin/busybox').chmod(0o755)
        root = pathlib.Path(w)/'state'

        def cmd(verb, *args, ok=True):
            return call(verb, '--root', root, *args, ok=ok)

        assert lines(cmd('create', '--rootfs', source, 'demo', '/bin/busybox', 'sh', '-c', 'read gate < /gate; echo WORKLOAD')) == ['geranium: created']
        gate = root/'demo/rootfs/gate'
        os.mkfifo(gate, 0o600)
        gatefd = os.open(gate, os.O_RDWR | os.O_CLOEXEC)
        p = subprocess.Popen([str(BASE/'gu-start'), '--root', str(root), 'demo'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert p.stderr is not None and p.stdout is not None
            assert bounded_line(p.stderr, 8) == b'geranium: started\n'
            assert p.poll() is None and not select.select([p.stdout], [], [], 0)[0]
            os.write(gatefd, b'release\n')
            stdout, stderr = p.communicate(timeout=8)
            assert stdout == b'WORKLOAD\n' and stderr == b'' and p.returncode == 0, (stdout, stderr, p.returncode)
        finally:
            os.close(gatefd)
            if p.poll() is None:
                p.kill()
            p.communicate(timeout=5)
        assert cmd('logs', 'demo').stdout == b'WORKLOAD\n'
        assert cmd('logs', 'demo').stderr == b''
        assert lines(cmd('start', '-d', 'demo')) == ['geranium: started (detached)']
        assert lines(cmd('stop', 'demo')) == ['geranium: stopped']
        assert lines(cmd('rm', 'demo')) == ['geranium: removed container']
        assert lines(cmd('run', '-d', '--rootfs', source, 'det', '/bin/busybox', 'sleep', '10')) == ['geranium: started (detached)']
        assert lines(cmd('stop', 'det')) == ['geranium: stopped']
        assert lines(cmd('rm', 'det')) == ['geranium: removed container']
        failed = cmd('run', '--rootfs', source, 'bad', '/bin/busybox', 'sh', '-c', 'exit 7', ok=False)
        assert failed.returncode == 7 and failed.stdout == b'' and failed.stderr == b'geranium: started\n', failed.stderr
        assert lines(cmd('rm', 'bad')) == ['geranium: removed container']
        rejected = cmd('run', '--rootfs', source, 'noexec', '/bin/missing', ok=False)
        assert rejected.returncode != 0 and b'started' not in rejected.stderr, rejected.stderr
        assert lines(cmd('rm', 'noexec')) == ['geranium: removed container']
        cmd('create', '--rootfs', source, 'pending', '/bin/busybox', 'true')
        (root/'pending').rename(root/'.pending-pending')
        assert lines(cmd('rm', 'pending')) == ['geranium: removed pending container']
    print('output namespace lifecycle/foreground/exit-code/startup-rejection PASS')


if __name__ == '__main__':
    pull_progress()
    pull_live()
    mutation()
    runtime()
