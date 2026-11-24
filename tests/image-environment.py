#!/usr/bin/env python3
"""Images get container defaults, never the manager's environment."""

import os
import tempfile
from pathlib import Path

from images import Registry, call, tar

DEFAULT_PATH = b"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def main():
    busybox = Path(
        os.environ.get("GERANIUM_TEST_BUSYBOX", "/usr/bin/busybox")
    ).read_bytes()
    cases = (
        ([], {b"HOME": b"/root", b"PATH": DEFAULT_PATH}),
        (["HOME=/custom", "PATH=/bin"], {b"HOME": b"/custom", b"PATH": b"/bin"}),
        (["HOME=", "PATH="], {b"HOME": b"", b"PATH": b""}),
    )
    for environment, expected in cases:
        with (
            tempfile.TemporaryDirectory() as state,
            Registry(
                [tar([("bin/busybox", busybox)])],
                config={
                    "WorkingDir": "/",
                    "Env": environment,
                    "Cmd": ["/bin/busybox", "env"],
                },
            ) as registry,
        ):
            call("pull", "--root", state, "--http-loopback", registry.ref)
            result = call(
                "run",
                "--root",
                state,
                "--image",
                registry.ref,
                "env",
                env=dict(
                    os.environ,
                    HOME="/manager-only-home",
                    GERANIUM_MANAGER_SECRET="must-not-leak",
                ),
            )
            actual = dict(line.split(b"=", 1) for line in result.stdout.splitlines())
            assert actual == expected, actual
            call("rm", "--root", state, "env")
    print("default HOME/PATH and explicit overrides without host environment PASS")


if __name__ == "__main__":
    main()
