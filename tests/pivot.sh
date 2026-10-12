#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin" "$w/fs/.geranium-old"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
printf 'preserved\n' > "$w/fs/.geranium-old/data"
./gu-run --root "$w/state" --rootfs "$w/fs" initial /bin/busybox cat /.geranium-old/data > "$w/out"
test "$(cat "$w/out")" = preserved
./gu-start --root "$w/state" initial > "$w/out"
test "$(cat "$w/out")" = preserved
rm -rf "$w/fs/.geranium-old"
./gu-run --root "$w/state" --rootfs "$w/fs" created /bin/busybox sh -c '/bin/busybox mkdir -p /.geranium-old; echo workload > /.geranium-old/data'
./gu-start --root "$w/state" created
test "$(cat "$w/state/created/rootfs/.geranium-old/data")" = workload
echo 'scratchless pivot initial directory and workload restart PASS'
