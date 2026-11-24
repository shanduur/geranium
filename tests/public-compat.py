#!/usr/bin/env python3
"""Exercise the reported Docker Hub images through pull and real execution."""

import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

BASE = Path.cwd()


def command(verb, state, *args):
    result = subprocess.run(
        [str(BASE / ("gu-" + verb)), "--root", str(state), *args],
        capture_output=True,
        timeout=600,
    )
    assert result.returncode == 0, (
        verb,
        args,
        result.returncode,
        result.stdout,
        result.stderr,
    )
    return result


def main():
    cases = (
        (
            "golang:latest",
            'export GOMAXPROCS=2; go version; printf \'package main\nimport "fmt"\nfunc main(){fmt.Println("geranium-go-pass")}\n\' > /tmp/geranium.go; go run -p=2 /tmp/geranium.go',
            b"geranium-go-pass\n",
        ),
        (
            "alpine:edge",
            "cat /etc/alpine-release; test \"$(id -u)\" = 0; printf 'geranium-alpine-pass\\n'",
            b"geranium-alpine-pass\n",
        ),
    )
    failures = []
    for reference, script, marker in cases:
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state"
            try:
                pulled = command("pull", state, reference)
                print(pulled.stderr.decode(), end="", flush=True)
                manifest = next(state.glob(".images/*/manifest.json"))
                config = json.loads(manifest.with_name("config.json").read_bytes())
                print(
                    reference,
                    "sha256:" + hashlib.sha256(manifest.read_bytes()).hexdigest(),
                    config["os"],
                    config["architecture"],
                    flush=True,
                )
                first = command(
                    "run", state, "--image", reference, "demo", "/bin/sh", "-ec", script
                )
                assert first.stdout.endswith(marker), first.stdout
                if reference == "golang:latest":
                    assert first.stdout.startswith(b"go version go"), first.stdout
                print(first.stdout.decode(), end="", flush=True)
                assert command("logs", state, "demo").stdout == first.stdout
                command("rmi", state, reference)
                restarted = command("start", state, "demo")
                assert restarted.stdout == first.stdout, (
                    first.stdout,
                    restarted.stdout,
                )
                assert b"status=exited" in command("inspect", state, "demo").stdout
                command("rm", state, "demo")
                print(
                    reference,
                    "public HTTPS pull/run/logs/rmi/restart/rm PASS",
                    flush=True,
                )
            except (AssertionError, subprocess.TimeoutExpired) as error:
                failures.append((reference, str(error)))
                print(reference, "FAIL", error, flush=True)
    assert not failures, failures


if __name__ == "__main__":
    main()
