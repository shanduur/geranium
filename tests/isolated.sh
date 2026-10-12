#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
printf 'namespace-test\n' > "$work/rootfs/marker"
"$bin" --root "$work/state" create --rootfs "$work/rootfs" demo /bin/busybox cat /marker
output=$("$bin" --root "$work/state" start demo)
[ "$output" = namespace-test ] || { printf 'wrong isolated output: %s\n' "$output" >&2; exit 1; }
"$bin" --root "$work/state" inspect demo | grep 'status=exited' >/dev/null
"$bin" --root "$work/state" create --rootfs "$work/rootfs" failing /missing-command
if "$bin" --root "$work/state" start failing; then printf 'missing exec succeeded\n' >&2; exit 1; fi
printf 'isolated start PASS\n'
