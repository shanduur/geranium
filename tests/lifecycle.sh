#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir "$work/rootfs"
printf 'hello\n' > "$work/rootfs/marker"
"$bin" --root "$work/state" create --rootfs "$work/rootfs" demo /bin/sh -c 'cat /marker' >/dev/null
"$bin" --root "$work/state" inspect demo | grep 'status=created' >/dev/null
[ -f "$work/state/demo/rootfs/marker" ]
"$bin" --root "$work/state" rm demo
[ ! -e "$work/state/demo" ]
if "$bin" --root "$work/state" create --rootfs "$work/rootfs" '../bad' /bin/true >/dev/null 2>&1; then
    printf 'unsafe name accepted\n' >&2; exit 1
fi
printf 'lifecycle metadata PASS\n'
