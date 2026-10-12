#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
for verb in create start stop rm inspect; do
    sh tests/fault-command.sh "$work/gu-$verb" "$verb" tests/command-lock-fixture.c -Wl,--wrap=read -Wl,--wrap=unlinkat -Wl,--wrap=setpgid -Wl,--wrap=fcntl
done
python3 tests/command-lock.py "$work"
