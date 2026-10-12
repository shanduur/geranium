#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
sh tests/fault-command.sh "$w/run" run tests/close-budget.c -Wl,--wrap=close
printf 'bounded close fixture soft limit: '
ulimit -n
"$w/run" --root "$w/state" --rootfs "$w/fs" demo /bin/busybox echo bounded
./gu-inspect --root "$w/state" demo | grep status=exited
[ "$(cat "$w/state/demo/exitcode")" = 0 ]
echo 'per-process close calls <=128 PASS'
