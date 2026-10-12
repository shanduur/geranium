#!/bin/sh
set -eu
w=$(mktemp -d)
client= reader=
trap 'for p in "$client" "$reader"; do if [ -n "$p" ]; then kill -KILL "$p" 2>/dev/null || :; fi; done; rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
mkfifo "$w/blocked"
sleep 30 < "$w/blocked" & reader=$!
./gu-run --root "$w/state" --rootfs "$w/fs" generation /bin/busybox sh -c '/bin/busybox yes output | /bin/busybox head -c 200000' > "$w/blocked" & client=$!
i=0
until ./gu-inspect --root "$w/state" generation 2>/dev/null | grep status=exited >/dev/null; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .05; done
kill -0 "$client"
if ./gu-start --root "$w/state" -d generation; then echo 'restart crossed unfinished foreground generation'; exit 1; fi
if ./gu-rm --root "$w/state" generation; then echo 'rm crossed unfinished foreground generation'; exit 1; fi
kill -KILL "$client"; wait "$client" 2>/dev/null || :; client=
./gu-rm --root "$w/state" generation
echo 'foreground generation fencing PASS'
