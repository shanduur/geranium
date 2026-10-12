#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/source" "$work/source/private" "$work/source/locked"
chmod 0500 "$work/source/locked"
mkfifo "$work/source/fifo"
if "$bin" --root "$work/state" create --rootfs "$work/source" demo /bin/true >/dev/null 2>&1; then echo 'accepted FIFO' >&2; exit 1; fi
if [ -e "$work/state/demo" ]; then echo 'failed create stranded name' >&2; exit 1; fi
rm "$work/source/fifo"
if "$bin" --root "$work/source/state" create --rootfs "$work/source" recurse /bin/true >/dev/null 2>&1; then echo 'accepted recursive source/state' >&2; exit 1; fi
"$bin" --root "$work/state" create --rootfs "$work/source" demo /bin/true
"$bin" --root "$work/state" rm demo
[ ! -e "$work/state/demo" ]
ln "$work/source/private" "$work/source/link" 2>/dev/null || :
: > "$work/source/one"
ln "$work/source/one" "$work/source/two"
if "$bin" --root "$work/state" create --rootfs "$work/source" linked /bin/true >/dev/null 2>&1; then echo 'accepted hardlinks' >&2; exit 1; fi
[ ! -e "$work/state/linked" ]
if env -u HOME "$bin" --root "$work/state" inspect missing 2>&1 | grep 'HOME missing' >/dev/null; then echo 'explicit root requires HOME' >&2; exit 1; fi
printf 'storage safety PASS\n'
