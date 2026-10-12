#!/bin/sh
set -eu
w=$(mktemp -d)
trap './gu-stop --root "$w/state" race >/dev/null 2>&1 || :; rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$w/fs/bin/busybox"
for mode in term kill; do
    flag=
    if [ "$mode" = kill ]; then flag=-DKILL_RACE; fi
    sh tests/fault-command.sh "$w/stop" stop tests/stop-race.c $flag -Wl,--wrap=backend_signal
    ./gu-run --root "$w/state" -d --rootfs "$w/fs" race /bin/busybox sleep 30
    "$w/stop" --root "$w/state" race 2> "$w/wrapped"
    case $mode in term) signal=TERM;; kill) signal=KILL;; esac
    grep -qx "wrapped $signal observed ESRCH" "$w/wrapped"
    cat "$w/wrapped"
    ./gu-inspect --root "$w/state" race > "$w/inspect"
    grep -qx 'status=exited' "$w/inspect"
    ./gu-rm --root "$w/state" race
 done
printf 'stop exit races: PASS\n'
