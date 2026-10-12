#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
"$bin" --root "$work/state" create --rootfs "$work/rootfs" demo /bin/busybox sleep 30
"$bin" --root "$work/state" start -d demo
"$bin" --root "$work/state" inspect demo | grep 'status=running' >/dev/null
if ! timeout 4 "$bin" --root "$work/state" stop demo; then echo 'stop failed/blocked' >&2; exit 1; fi
"$bin" --root "$work/state" inspect demo | grep 'status=exited' >/dev/null
"$bin" --root "$work/state" rm demo
printf 'detached stop PASS\n'
