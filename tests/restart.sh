#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin" "$work/rootfs/data"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
ln -s "$work/rootfs" "$work/link"
if "$bin" --root "$work/state" create --rootfs "$work/link" invalid /bin/busybox true >/dev/null 2>&1; then
    printf 'symlink rootfs accepted\n' >&2; exit 1
fi
"$bin" --root "$work/state" create --rootfs "$work/rootfs" demo /bin/busybox touch /data/persist
"$bin" --root "$work/state" start demo
[ -f "$work/state/demo/rootfs/data/persist" ]
"$bin" --root "$work/state" start demo
[ -f "$work/state/demo/rootfs/data/persist" ]
printf 'restart persists PASS\n'
