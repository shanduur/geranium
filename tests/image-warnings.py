#!/usr/bin/env python3
"""Unsupported optional runtime metadata warns without making pull fail."""

import os
import pathlib
import tempfile

from images import Registry, call, published_roots, tar


def main():
    cases = (
        ("User", "1000:1000"),
        ("ExposedPorts", {"8080/tcp": {}}),
        ("Volumes", {"/data": {}}),
        ("Healthcheck", {"Test": ["CMD", "/bin/true"]}),
        ("StopSignal", "SIGQUIT"),
    )
    for field, value in cases:
        with (
            tempfile.TemporaryDirectory() as state,
            Registry(config={field: value}) as registry,
        ):
            first = call("pull", "--root", state, "--http-loopback", registry.ref)
            assert b"warning:" in first.stderr, first.stderr
            assert field.encode() in first.stderr, first.stderr
            assert len(published_roots(state)) == 1
            # A cache hit must retain both the config and its visible limitation.
            cached = call("pull", "--root", state, "--http-loopback", registry.ref)
            assert b"warning:" in cached.stderr, cached.stderr
            assert field.encode() in cached.stderr, cached.stderr
            assert len(published_roots(state)) == 1

    busybox = pathlib.Path(
        os.environ.get("GERANIUM_TEST_BUSYBOX", "/usr/bin/busybox")
    ).read_bytes()
    for user in ("1000", "nobody:nogroup", "root:root", "0:0"):
        with (
            tempfile.TemporaryDirectory() as state,
            Registry(
                [tar([("bin/busybox", busybox)])],
                config={
                    "User": user,
                    "WorkingDir": "/",
                    "Cmd": ["/bin/busybox", "id", "-u"],
                },
            ) as registry,
        ):
            call("pull", "--root", state, "--http-loopback", registry.ref)
            result = call("run", "--root", state, "--image", registry.ref, "identity")
            assert result.stdout == b"0\n", result.stdout
            if user in ("1000", "nobody:nogroup"):
                assert b"warning:" in result.stderr and b"User" in result.stderr
                assert b"mapped to the caller" in result.stderr, result.stderr
            call("rm", "--root", state, "identity")
    print("unsupported image metadata warns; User adaptation is explicit PASS")


if __name__ == "__main__":
    main()
