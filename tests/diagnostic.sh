#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'chmod 700 "$w/state/denied/rootfs" 2>/dev/null || :; rm -rf "$w"' 0
mkdir "$w/fs"
./gu-create --root "$w/state" --rootfs "$w/fs" missing /missing-command
set +e
./gu-start --root "$w/state" missing > "$w/out" 2> "$w/error"
rc=$?
set -e
[ "$rc" = 127 ]
grep /missing-command "$w/error" >/dev/null || { echo 'exec failure lacks actionable command diagnostic'; exit 1; }
./gu-inspect --root "$w/state" missing | grep status=exited
./gu-rm --root "$w/state" missing
./gu-create --root "$w/state" --rootfs "$w/fs" denied /missing-command
chmod 000 "$w/state/denied/rootfs"
set +e
./gu-start --root "$w/state" denied > "$w/out" 2> "$w/error"
rc=$?
set -e
chmod 700 "$w/state/denied/rootfs"
printf 'chmod-000 kernel attempt returned %s\n' "$rc"
cat "$w/error"
if [ "$rc" != 125 ] || ! grep -E '(chdir rootfs|bind rootfs): Permission denied' "$w/error"; then
    echo 'kernel allowed mode-000 bootstrap; testing explicitly injected chdir EACCES'
    sh tests/fault-command.sh "$w/start-denied" start tests/chdir-denied.c -Wl,--wrap=chdir
    set +e
    "$w/start-denied" --root "$w/state" denied > "$w/out" 2> "$w/error"
    rc=$?
    set -e
    cat "$w/error"
fi
[ "$rc" = 125 ]
grep -E '(chdir rootfs|bind rootfs): Permission denied' "$w/error"
[ "$(wc -c < "$w/state/denied/log")" -le 512 ]
./gu-rm --root "$w/state" denied
echo 'exec and bootstrap diagnostics PASS'
