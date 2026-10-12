#!/bin/sh
set -eu
w=$(mktemp -d)
trap './gu-stop --root "$w/state" fence >/dev/null 2>&1 || :; rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
./gu-run --root "$w/state" -d --rootfs "$w/fs" fence /bin/busybox sh -c 'echo ready; /bin/busybox sleep 30'
i=0
until ./gu-logs --root "$w/state" fence | grep ready >/dev/null; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .05; done
# Runtime ownership must fence transitions even if metadata says exited.
printf 'exited\n' > "$w/state/fence/status"
if ./gu-start --root "$w/state" -d fence; then echo 'accepted occupied runtime'; exit 1; fi
./gu-logs --root "$w/state" fence | grep ready >/dev/null || { echo 'occupied start changed current log'; exit 1; }
if ./gu-rm --root "$w/state" fence; then echo 'removed occupied runtime'; exit 1; fi
./gu-stop --root "$w/state" fence
./gu-rm --root "$w/state" fence
echo 'runtime publication fencing PASS'
