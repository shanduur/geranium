#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
sh tests/fault-command.sh "$work/create" create tests/image-copy-lock.c -Wl,--wrap=read
python3 tests/images.py race "$work/create"
