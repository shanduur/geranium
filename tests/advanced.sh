#!/bin/sh
set -eu
bin=./tests/cli.sh
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin" "$w/fs/proc"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
# Observable namespace identity and host-root exclusion.
"$bin" --root "$w/state" run --rootfs "$w/fs" isolation /bin/busybox sh -c 'test ! -e /etc/hostname || exit 1; echo $$' > "$w/out"
[ "$(cat "$w/out")" = 2 ]
"$bin" --root "$w/state" run -d --rootfs "$w/fs" graceful /bin/busybox sh -c 'trap "echo TERM-seen; exit 42" TERM; echo ready; while :; do /bin/busybox sleep 1; done'
i=0
until "$bin" --root "$w/state" logs graceful | grep ready >/dev/null; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .05; done
"$bin" --root "$w/state" stop graceful
"$bin" --root "$w/state" logs graceful | grep TERM-seen
[ "$(cat "$w/state/graceful/exitcode")" = 42 ]
"$bin" --root "$w/state" run -d --rootfs "$w/fs" forced /bin/busybox sh -c 'trap "" TERM; echo ready; while :; do /bin/busybox sleep 1; done'
until "$bin" --root "$w/state" logs forced | grep ready >/dev/null; do sleep .05; done
"$bin" --root "$w/state" stop forced
"$bin" --root "$w/state" inspect forced | grep status=exited
[ ! -e "$w/state/forced/exitcode" ]
"$bin" --root "$w/state" run -d --rootfs "$w/fs" noisy /bin/busybox sh -c 'while :; do echo noisy-output; done'
sleep .3
timeout 5 "$bin" --root "$w/state" stop noisy
[ "$(wc -c < "$w/state/noisy/log")" -le 1048576 ]
# Recovery after failed exec, including retry.
"$bin" --root "$w/state" create --rootfs "$w/fs" failed /missing
set +e
"$bin" --root "$w/state" start failed
rc=$?
set -e
[ "$rc" = 127 ]
"$bin" --root "$w/state" inspect failed | grep status=exited
set +e
"$bin" --root "$w/state" start failed
rc=$?
set -e
[ "$rc" = 127 ]
# A foreign bare PID file cannot authorize a signal.
printf '%s\n' $$ > "$w/state/failed/pid"
if "$bin" --root "$w/state" stop failed; then exit 1; fi
"$bin" --root "$w/state" rm failed
echo 'advanced lifecycle PASS'
