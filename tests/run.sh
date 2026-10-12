#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
"$bin" --root "$work/state" run --rootfs "$work/rootfs" demo /bin/busybox echo hello > "$work/out"
[ "$("$bin" --root "$work/state" logs demo)" = hello ]
[ "$("$bin" --root "$work/state" inspect demo | grep '^status=')" = status=exited ]
[ "$("$bin" --root "$work/state" logs demo)" = "$(tr -d '\n' < "$work/out")" ]
printf 'run and logs PASS\n'
