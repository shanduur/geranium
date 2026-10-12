#!/bin/sh
set -eu
w=$(mktemp -d)
trap './gu-stop --root "$w/state" demo >/dev/null 2>&1 || :; rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/session.c ${LDFLAGS:-} ${LDLIBS:-} -o "$w/session"
"$w/session" "$w/state" "$PWD/gu-run" --root "$w/state" -d --rootfs "$w/fs" demo /bin/busybox sleep 30
./gu-inspect --root "$w/state" demo | grep status=running
./gu-stop --root "$w/state" demo
echo 'detached caller process group independence PASS'
