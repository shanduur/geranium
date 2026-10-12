#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$w/fs/bin/busybox"
for fault in preexec-kill trace-denied; do
    case $fault in preexec-kill) symbol=setpgid; expected=137;; trace-denied) symbol=backend_exec_prepare; expected=125;; esac
    sh tests/fault-command.sh "$w/run" run "tests/$fault.c" -Wl,--wrap="$symbol"
    set +e
    "$w/run" --root "$w/state" -d --rootfs "$w/fs" "$fault" /bin/busybox echo EXECUTED > "$w/out" 2> "$w/error"
    rc=$?
    set -e
    cat "$w/error"
    [ "$rc" = "$expected" ]
    [ "$(cat "$w/state/$fault/exitcode")" = "$expected" ]
    ./gu-inspect --root "$w/state" "$fault" | grep '^status=exited$'
    if grep EXECUTED "$w/state/$fault/log"; then exit 1; fi
    if [ "$fault" = trace-denied ]; then grep 'exec readiness ptrace prepare: Operation not permitted' "$w/state/$fault/log"; fi
    # rm must acquire the runtime lock, proving namespace init released ownership.
    ./gu-rm --root "$w/state" "$fault"
    [ ! -e "$w/state/$fault" ]
    printf '%s detached exit %s, marker absent, runtime released: PASS\n' "$fault" "$rc"
done
# An exec followed immediately by exit is still a successful detached startup.
i=0
while [ "$i" -lt 20 ]; do
    ./gu-run --root "$w/state" -d --rootfs "$w/fs" fast /bin/busybox echo EXECUTED
    j=0
    while [ "$(cat "$w/state/fast/status")" != exited ]; do
        j=$((j+1)); [ "$j" -lt 100 ]; sleep .02
    done
    [ "$(cat "$w/state/fast/exitcode")" = 0 ]
    [ "$(cat "$w/state/fast/log")" = EXECUTED ]
    ./gu-rm --root "$w/state" fast
    i=$((i+1))
done
printf '20 immediate successful exec/exit launches: PASS\n'
# Failed exec preserves its public error code and completed result publication.
chmod 600 "$w/fs/bin/busybox"
for kind in missing denied; do
    case $kind in missing) command=/missing; expected=127;; denied) command=/bin/busybox; expected=126;; esac
    set +e
    ./gu-run --root "$w/state" -d --rootfs "$w/fs" "$kind" "$command" > "$w/out" 2> "$w/error"
    rc=$?
    set -e
    [ "$rc" = "$expected" ]
    [ "$(cat "$w/state/$kind/status")" = exited ]
    [ "$(cat "$w/state/$kind/exitcode")" = "$expected" ]
    grep "$command:" "$w/error"
    ./gu-rm --root "$w/state" "$kind"
    printf '%s exec failure %s, result published: PASS\n' "$kind" "$rc"
done
