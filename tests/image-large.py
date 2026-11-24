#!/usr/bin/env python3
"""End-to-end descriptor larger than the old 64 MiB layer cap."""

import gzip
import os
import tempfile

import images


def test_large_layer():
    # Incompressible payload ensures the *compressed descriptor* exceeds 64 MiB.
    payload = os.urandom(65 * 1024 * 1024)
    layer = images.tar([("large", payload)])
    assert len(gzip.compress(layer, compresslevel=1)) > 64 * 1024 * 1024
    with tempfile.TemporaryDirectory() as root, images.Registry([layer]) as registry:
        result = images.call("pull", "--root", root, "--http-loopback", registry.ref)
        assert result.returncode == 0
        roots = images.published_roots(root)
        assert len(roots) == 1
        assert (roots[0] / "large").read_bytes() == payload
    print("compressed descriptor larger than 64 MiB pulled and verified PASS")


if __name__ == "__main__":
    test_large_layer()
