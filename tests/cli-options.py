#!/usr/bin/env python3
"""Exercise real separate CLI binaries without touching default user state."""
import os
import pathlib
import subprocess
import tempfile

BIN = pathlib.Path(__file__).resolve().parent.parent


def invoke(verb, *args):
    return subprocess.run([str(BIN / ('gu-' + verb)), *map(str, args)],
                          capture_output=True, text=True,
                          env=dict(os.environ, HOME=str(home)))


def rejected(verb, expected, *args):
    result = invoke(verb, *args)
    assert result.returncode != 0, (verb, args, result)
    assert expected in result.stderr, (verb, args, result)


with tempfile.TemporaryDirectory(prefix='geranium-cli-options-') as tmp:
    home = pathlib.Path(tmp) / 'home'
    home.mkdir()
    root = pathlib.Path(tmp) / 'state'
    rootfs = pathlib.Path(tmp) / 'rootfs'
    rootfs.mkdir()
    for verb in ('create', 'run', 'start', 'stop', 'inspect', 'logs', 'rm', 'pull', 'rmi'):
        result = invoke(verb, '--root=' + str(root), '--help')
        assert result.returncode == 0 and 'usage: gu-' + verb in result.stdout, (verb, result)
        result = invoke(verb, '--root', '--help')
        assert result.returncode == 0 and 'usage: gu-' + verb in result.stdout, (verb, result)
        rejected(verb, 'unknown option', '--root=' + str(root), '--bogus')
        rejected(verb, 'unknown option', '--roo=' + str(root), '--help')
        rejected(verb, 'requires', '--root')
        rejected(verb, 'unknown option', '--root=' + str(root), '-z')
        rejected(verb, 'unknown option -r', '--root=' + str(root), '-r')
        rejected(verb, 'unknown option -xz', '--root=' + str(root), '-xz')
        assert invoke(verb, '--root=' + str(root), '-h').returncode == 0, verb
        if verb in ('create', 'run'):
            rejected(verb, 'unknown option -f', '--root=' + str(root), '-f')
            rejected(verb, 'unknown option -i', '--root=' + str(root), '-i')
        if verb == 'pull':
            rejected(verb, 'unknown option -l', '--root=' + str(root), '-l')
        if verb not in ('create', 'run'):
            rejected(verb, 'invalid image reference' if verb in ('pull', 'rmi') else 'unexpected',
                     '--root=' + str(root), '--', '--bogus')
    for verb in ('create', 'run'):
        rejected(verb, 'source', '--root=' + str(root), '--rootfs', rootfs, '--image', 'other', 'test', '/bin/true')
        rejected(verb, 'source', '--root=' + str(root), '--rootfs', rootfs, '--rootfs', rootfs, 'test', '/bin/true')
        rejected(verb, 'requires', '--root=' + str(root), '--image')
        rejected(verb, 'unknown option', '--root=' + str(root), '--rootfs', rootfs, '--bogus', 'test', '/bin/true')
    rejected('create', 'unknown option', '--root=' + str(root), '-d', '--rootfs', rootfs, 'test', '/bin/true')
    rejected('pull', 'unexpected', '--root=' + str(root), '--http-loopback', 'one', 'two')
    bad_root = pathlib.Path(tmp) / 'not-a-directory'
    bad_root.touch()
    incomplete = (('create', ('--rootfs', str(rootfs))),
                  ('create', ('--image', 'example:latest')),
                  ('run', ('--rootfs', str(rootfs))),
                  ('run', ('--image', 'example:latest')),
                  ('run', ('-d',)), ('start', ('-d',)),
                  ('pull', ('--http-loopback',)))
    for index, (verb, modifiers) in enumerate(incomplete):
        fresh_root = pathlib.Path(tmp) / ('incomplete-' + str(index))
        result = invoke(verb, '--root', fresh_root, *modifiers)
        assert result.returncode != 0 and 'missing arguments' in result.stderr, (verb, modifiers, result)
        assert fresh_root.is_dir(), (verb, modifiers, result)
        result = invoke(verb, '--root', bad_root, *modifiers)
        assert result.returncode != 0 and 'open path component' in result.stderr, (verb, modifiers, result)
    for verb in ('create', 'run', 'start', 'pull'):
        bare_root = pathlib.Path(tmp) / ('bare-' + verb)
        rejected(verb, 'missing arguments', '--root', bare_root)
        assert not bare_root.exists(), verb
    payload = ('/bin/true', '--bogus', '--root', '/not-the-state-root', '--help', '--', '-c')
    result = invoke('create', '--rootfs=' + str(rootfs), '--root=' + str(root), 'sample', *payload)
    assert result.returncode == 0, result
    assert (root / 'sample' / 'command').read_bytes() == b'\0'.join(part.encode() for part in payload) + b'\0'
    assert (home / '.local/state/geranium').is_dir()
    result = invoke('inspect', '--root=' + str(root), '--', 'sample')
    assert result.returncode == 0 and 'status=created' in result.stdout, result
    result = invoke('rm', '--root=' + str(root), 'sample')
    assert result.returncode == 0, result
print('cli getopt options PASS')
