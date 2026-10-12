#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
sh tests/fault-command.sh "$w/rm" rm tests/removal.c -Wl,--wrap=readdir -Wl,--wrap=renameat -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=closedir -Wl,--wrap=unlinkat
python3 tests/removal.py "$w"
