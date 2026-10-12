"""Real CLI races; syscall wrappers only pause at controlled scheduling points."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

fixture = pathlib.Path(sys.argv[1]).resolve()
production = pathlib.Path(os.environ.get('GERANIUM_BINDIR', '.')).resolve()
children = []
latencies = []
work = pathlib.Path(tempfile.mkdtemp(prefix='geranium-command-lock-'))
state = work / 'state'
fs = work / 'fs'
(fs / 'bin').mkdir(parents=True)
shutil.copyfile(os.environ.get('GERANIUM_TEST_BUSYBOX', shutil.which('busybox')), fs / 'bin/busybox')
(fs / 'bin/busybox').chmod(0o755)
(fs / 'stall-copy').write_text('copied payload\n')

def launch(verb, *args, wrapped=False, env=None, stdout=subprocess.PIPE):
    process = subprocess.Popen([str((fixture if wrapped else production) / ('gu-' + verb)),
                                '--root', str(state), *map(str, args)],
                               env=dict(os.environ, **(env or {})), stdout=stdout, stderr=subprocess.PIPE)
    process.launched_at = time.monotonic()
    children.append(process)
    return process

def finish(process, expected=0, limit=2):
    try:
        out, err = process.communicate(timeout=limit)
    except subprocess.TimeoutExpired:
        raise AssertionError('command blocked beyond %.1fs: %s' % (limit, process.args))
    elapsed = time.monotonic() - process.launched_at
    print('command=%s exit=%s latency_ms=%.3f' % (' '.join(process.args), process.returncode, elapsed * 1000), flush=True)
    assert process.returncode == expected, (process.args, process.returncode, out, err)
    return out, elapsed

def cli(verb, *args):
    out, elapsed = finish(launch(verb, *args))
    latencies.append((verb, elapsed))
    return out

def wait_file(path):
    end = time.monotonic() + 3
    while not path.exists():
        assert time.monotonic() < end, ('fixture never reached scheduling point', path)
        time.sleep(.005)

def stalled(verb, name, mode, *args):
    ready, release = work / (name + mode + '.ready'), work / (name + mode + '.release')
    env = {'STALL_READY': str(ready), 'STALL_RELEASE': str(release), mode: str(fs / 'stall-copy')}
    if mode == 'STALL_START':
        ready, release = state / name / 'rootfs/start.ready', state / name / 'rootfs/start.release'
        env.update(STALL_READY='/start.ready', STALL_RELEASE='/start.release')
    process = launch(verb, *args, wrapped=True, env=env)
    wait_file(ready)
    assert process.poll() is None
    return process, release

def waiter(verb, tag, *args, env=None):
    ready = work / (tag + '.lock-ready')
    process = launch(verb, *args, wrapped=True, env=dict(env or {}, LOCK_READY=str(ready)))
    wait_file(ready)
    time.sleep(.15)
    assert process.poll() is None, ('same-name command failed to serialize', process.args)
    return process

def create(name):
    return cli('create', '--rootfs', fs, name, '/bin/busybox', 'sleep', '30')

def live_b():
    cli('run', '-d', '--rootfs', fs, 'B', '/bin/busybox', 'sh', '-c', 'echo live-B; /bin/busybox sleep 30')
    end = time.monotonic() + 2
    while b'live-B' not in cli('logs', 'B'):
        assert time.monotonic() < end
        time.sleep(.01)

def unrelated():
    assert b'status=running' in cli('inspect', 'B')
    assert b'live-B' in cli('logs', 'B')
    cli('stop', 'B')
    assert b'status=exited' in cli('inspect', 'B')
    cli('rm', 'B')

try:
    live_b()
    a, release = stalled('create', 'A', 'STALL_COPY', '--rootfs', fs, 'A', '/bin/busybox', 'sleep', '30')
    print('copy A reached controlled read stall; B is live', flush=True)
    probes = [(verb, launch(verb, 'B')) for verb in ('inspect', 'logs', 'stop')]
    deadline = time.monotonic() + 2
    while any(process.poll() is None for _, process in probes) and time.monotonic() < deadline:
        for _, process in probes:
            if process.poll() is not None and not hasattr(process, 'completed_at'):
                process.completed_at = time.monotonic()
        time.sleep(.005)
    for _, process in probes:
        if process.poll() is not None and not hasattr(process, 'completed_at'):
            process.completed_at = time.monotonic()
    blocked = [verb for verb, process in probes if process.poll() is None]
    for verb, process in probes:
        print('stalled A: B %s %s' % (verb, 'BLOCKED >=2000ms' if process.poll() is None else 'completed exit=%s latency_ms=%.3f' % (process.returncode, (process.completed_at - process.launched_at) * 1000)), flush=True)
    assert not blocked, ('cross-container global blocking', blocked)
    for verb, process in probes:
        out, elapsed = finish(process)
        if verb == 'logs':
            assert b'live-B' in out
    cli('rm', 'B')
    live_b()
    unrelated()
    duplicate = waiter('create', 'duplicate', '--rootfs', fs, 'A', '/bin/busybox', 'true')
    observer = waiter('inspect', 'observer', 'A')
    assert not (state / 'A').exists(), 'incomplete container published'
    release.touch()
    finish(a)
    finish(duplicate, expected=1)
    assert b'status=created' in finish(observer)[0]
    assert (state / 'A/rootfs/stall-copy').read_text() == 'copied payload\n'
    anchor = state / '.command-A.lock'
    inode = anchor.stat().st_ino

    # A startup acknowledgement delay must not block live B.
    live_b()
    a, release = stalled('start', 'A', 'STALL_START', '-d', 'A')
    observer = waiter('inspect', 'starting-observer', 'A')
    unrelated()
    release.touch()
    finish(a)
    assert b'status=running' in finish(observer)[0]
    a, release = stalled('stop', 'A', 'STALL_STOP', 'A')
    observer = waiter('inspect', 'stopping-observer', 'A')
    live_b()
    unrelated()
    release.touch()
    finish(a)
    assert b'status=exited' in finish(observer)[0]

    # rm has unpublished A and is cleaning its private stage; queued create must wait.
    a, release = stalled('rm', 'A', 'STALL_RM', 'A')
    assert not (state / 'A').exists()
    assert (state / '.pending-A').is_dir()
    copy_ready, copy_release = work / 'replacement-copy.ready', work / 'replacement-copy.release'
    replacement = waiter('create', 'replacement', '--rootfs', fs, 'A', '/bin/busybox', 'sleep', '30',
                         env={'STALL_COPY': str(fs / 'stall-copy'), 'STALL_READY': str(copy_ready),
                              'STALL_RELEASE': str(copy_release)})
    live_b()
    unrelated()
    release.touch()
    finish(a)
    wait_file(copy_ready)
    assert anchor.stat().st_ino == inode
    # This fresh opener must join the same inode as the waiter that opened before rm.
    fresh = waiter('inspect', 'fresh-after-rm', 'A')
    copy_release.touch()
    finish(replacement)
    assert b'status=created' in finish(fresh)[0]
    assert anchor.stat().st_ino == inode, 'lock anchor changed across rm/recreate'
    assert b'status=created' in cli('inspect', 'A')

    # Aborted copy leaves staging; same-name retry cleans it under the same lock.
    a, release = stalled('create', 'retry', 'STALL_COPY', '--rootfs', fs, 'retry', '/bin/busybox', 'true')
    retry = waiter('create', 'retry-waiter', '--rootfs', fs, 'retry', '/bin/busybox', 'true')
    a.kill()
    finish(a, expected=-9)
    finish(retry)
    assert not (state / '.pending-retry').exists()
    assert (state / 'retry/rootfs/stall-copy').read_text() == 'copied payload\n'

    # A blocked stdout reader must not retain the command lock.
    cli('run', '-d', '--rootfs', fs, 'logger', '/bin/busybox', 'sh', '-c',
        '/bin/busybox yes X | /bin/busybox head -c 1048576; /bin/busybox sleep 30')
    end = time.monotonic() + 3
    while (state / 'logger/log').stat().st_size < 1048576:
        assert time.monotonic() < end
        time.sleep(.01)
    readfd, writefd = os.pipe()
    reader = launch('logs', 'logger', stdout=writefd)
    os.close(writefd)
    time.sleep(.15)
    assert reader.poll() is None
    assert b'status=running' in cli('inspect', 'logger')
    cli('stop', 'logger')
    cli('rm', 'logger')
    create('logger')
    os.close(readfd)
    finish(reader, expected=-13)
    rejected = work / 'rejected-state'
    rejected.mkdir(mode=0o700)
    rejected.chmod(0o777)
    for verb in ('create', 'run', 'start', 'stop', 'inspect', 'logs', 'rm'):
        args = ['--rootfs', str(fs), 'rejected', '/bin/busybox', 'true'] if verb in ('create', 'run') else ['rejected']
        result = subprocess.run([str(production / ('gu-' + verb)), '--root', str(rejected), *args],
                                capture_output=True, timeout=2)
        print('invalid-root %s exit=%s' % (verb, result.returncode), flush=True)
        assert result.returncode == 1 and b'state directory must be owned by you' in result.stderr
        assert not list(rejected.iterdir()), 'invalid root mutated before validation'

    # Every command uses the same validated anchor; reject unsafe anchors before
    # waiting or opening a container. Invalid root validation precedes anchors.
    for kind in ('symlink', 'directory', 'hardlink', 'public'):
        name = 'bad-' + kind
        anchor = state / ('.command-' + name + '.lock')
        target = work / ('outside-' + kind)
        target.write_text('untouched')
        if kind == 'symlink':
            anchor.symlink_to(target)
        elif kind == 'directory':
            anchor.mkdir()
        elif kind == 'hardlink':
            os.link(target, anchor)
        else:
            anchor.write_text('untouched')
            anchor.chmod(0o666)
        for verb in ('create', 'run', 'start', 'stop', 'inspect', 'logs', 'rm'):
            args = ('--rootfs', fs, name, '/bin/busybox', 'true') if verb in ('create', 'run') else (name,)
            finish(launch(verb, *args), expected=1)
        assert target.read_text() == 'untouched'
        assert not (state / name).exists()
    print('same-name create/start/stop, rm/recreate, stale-stage retry, blocked logs, unsafe anchors PASS')
    for verb, elapsed in latencies:
        print('%s exit=0 latency_ms=%.3f' % (verb, elapsed * 1000))
finally:
    for process in children:
        if process.poll() is None:
            process.kill()
        process.wait()
    for name in ('A', 'B', 'logger', 'retry'):
        subprocess.run([str(production / 'gu-stop'), '--root', str(state), name],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=6)
    shutil.rmtree(work)
