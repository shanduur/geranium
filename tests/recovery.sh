#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
"$bin" --root "$work/state" create --rootfs "$work/rootfs" demo /bin/busybox true
printf 'interrupted write\n' > "$work/state/demo/status.tmp"
"$bin" --root "$work/state" start demo
"$bin" --root "$work/state" inspect demo | grep '^status=exited$' >/dev/null
printf 'stale temporary status recovery PASS\n'
