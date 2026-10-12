#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
sh tests/fault-command.sh "$w/completion" run tests/completion.c -Wl,--wrap=openat -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=renameat
sh tests/fault-command.sh "$w/tail" run tests/completion-tail.c -Wl,--wrap=read
python3 tests/completion.py "$w"
