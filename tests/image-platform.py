#!/usr/bin/env python3
"""Real image pull and open behavior with a private production backend dispatcher."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

from images import Registry, OCI, descriptor, encode


pull, create, probe = map(pathlib.Path, sys.argv[1:4])
arch = sys.argv[4]
assert arch in ('riscv64', 'ppc64le', 's390x'), arch
unknown_pull = pathlib.Path(sys.argv[5]) if len(sys.argv) == 6 else None


def run(binary, *args):
    return subprocess.run([str(binary), *map(str, args)], capture_output=True, timeout=20)


def failed(process, *parts):
    assert process.returncode != 0, (process.args, process.stderr)
    for part in parts:
        assert part.encode() in process.stderr, process.stderr
    assert process.stderr.endswith(b'\n'), process.stderr


def indexed(registry, platforms):
    original = registry.manifest
    manifest = json.loads(original)
    entries = [dict(descriptor(original, OCI + 'manifest.v1+json'), platform=p)
               for p in platforms]
    registry.manifest = encode(dict(schemaVersion=2, mediaType=OCI + 'index.v1+json', manifests=entries))
    original_route = registry.route
    def route(path, headers):
        if path.endswith('/manifests/latest'):
            return 200, {'Content-Type': OCI + 'index.v1+json'}, registry.manifest
        if path.endswith('/manifests/sha256:' + hashlib.sha256(original).hexdigest()):
            return 200, {'Content-Type': OCI + 'manifest.v1+json'}, original
        return original_route(path, headers)
    registry.route = route
    return original


def set_config_arch(registry, architecture):
    config = encode(dict(registry.config, architecture=architecture))
    registry.blobs['sha256:' + hashlib.sha256(config).hexdigest()] = config
    manifest = json.loads(registry.manifest)
    manifest['config'] = descriptor(config, OCI + 'config.v1+json')
    registry.manifest = encode(manifest)


assert run(probe).stdout.strip() == arch.encode(), f'production backend dispatcher did not map {arch}'
print(f'production backend dispatcher simulated {arch} PASS', flush=True)
with tempfile.TemporaryDirectory() as root:
    p = run(create, '--root', root, '--image', 'alpine:edge', 'platform-test')
    failed(p, 'alpine:edge', 'gu-pull', '--root')
    assert b'<same-root>' not in p.stderr, p.stderr
    print('missing image recovery PASS', flush=True)

for platforms in [
    [dict(os='linux', architecture='not-the-target')],
    [dict(os='linux', architecture=arch, variant='v64')],
]:
    with tempfile.TemporaryDirectory() as root:
        reg = Registry()
        set_config_arch(reg, arch)
        indexed(reg, platforms)
        with reg:
            failed(run(pull, '--root', root, '--http-loopback', reg.ref), 'linux/' + arch)
print(f'{arch} no matching platform and strict variant PASS', flush=True)

with tempfile.TemporaryDirectory() as root:
    reg = Registry()
    set_config_arch(reg, arch)
    indexed(reg, [dict(os='linux', architecture=arch)])
    with reg:
        p = run(pull, '--root', root, '--http-loopback', reg.ref)
        assert p.returncode == 0, p.stderr
        p = run(create, '--root', root, '--image', reg.ref, 'platform-test')
        assert p.returncode == 0, p.stderr
        roots = list(pathlib.Path(root).glob('.images/*/rootfs'))
        assert len(roots) == 1
        roots[0].rename(roots[0].with_name('missing-rootfs'))
        failed(run(create, '--root', root, '--image', reg.ref, 'platform-test2'), 'rootfs', reg.ref)
        roots[0].write_text('corrupt')
        failed(run(create, '--root', root, '--image', reg.ref, 'platform-test3'), 'invalid image rootfs', reg.ref)
print(f'indexed {arch} pull/config/open and missing rootfs PASS', flush=True)

with tempfile.TemporaryDirectory() as root:
    reg = Registry()
    # Explicit disagreement, independent of the test runner's native architecture.
    set_config_arch(reg, 'not-the-target')
    indexed(reg, [dict(os='linux', architecture=arch)])
    with reg:
        failed(run(pull, '--root', root, '--http-loopback', reg.ref), 'linux/' + arch)
print('config platform mismatch PASS', flush=True)

with tempfile.TemporaryDirectory() as root:
    reg = Registry()
    reg.manifest = encode(dict(schemaVersion=2, mediaType=OCI+'index.v1+json', manifests={}))
    route = reg.route
    reg.route = lambda path, headers: (200, {'Content-Type': OCI+'index.v1+json'}, reg.manifest) if path.endswith('/manifests/latest') else route(path, headers)
    with reg:
        failed(run(pull, '--root', root, '--http-loopback', reg.ref), 'index')
print('malformed index diagnostic PASS', flush=True)

with tempfile.TemporaryDirectory() as root:
    reg = Registry()
    indexed(reg, [dict(os='linux', architecture=arch), dict(os='linux', architecture=arch)])
    with reg:
        failed(run(pull, '--root', root, '--http-loopback', reg.ref), 'ambiguous index platform')
print('ambiguous matching platform remains rejected PASS', flush=True)

if unknown_pull is not None:
    for index in (False, True):
        with tempfile.TemporaryDirectory() as root:
            reg = Registry()
            if index:
                indexed(reg, [dict(os='linux', architecture=arch)])
            with reg:
                p = run(unknown_pull, '--root', root, '--http-loopback', reg.ref)
                failed(p, 'unsupported host architecture')
                assert b'expected linux/' not in p.stderr, p.stderr
    print('unknown host direct manifest and index PASS', flush=True)
