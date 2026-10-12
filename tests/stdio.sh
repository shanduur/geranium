#!/bin/sh
set -eu
work=$(mktemp -d)
trap './gu-stop --root "$work/state" demo >/dev/null 2>&1 || :; ./gu-stop --root "$work/signals" demo >/dev/null 2>&1 || :; rm -rf "$work"' 0
for verb in create start run stop inspect logs rm pull rmi; do
    (unset HOME; "./gu-$verb" --help >/dev/null; "./gu-$verb" -h >/dev/null)
    "./gu-$verb" --root "$work/unused" --help >/dev/null
done
test ! -e "$work/unused"
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$work/rootfs/bin/busybox"
chmod 755 "$work/rootfs/bin/busybox"
if ./gu-create --root "$work/names" --rootfs "$work/rootfs" --help /bin/busybox true; then
    printf 'flag-like container name was accepted\n' >&2
    exit 1
fi
test ! -e "$work/names/--help"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/capture.c ${LDFLAGS:-} ${LDLIBS:-} -o "$work/capture"
"$work/capture" "$PWD/gu-run" --root "$work/state" -d --rootfs "$work/rootfs" demo /bin/busybox yes
"$work/capture" --blocked-logs "$PWD" "$work/state"
"$work/capture" "$work/capture" --host-signals "$PWD/gu-run" --root "$work/signals" --rootfs "$work/rootfs" demo /bin/busybox echo signal-safe
printf 'help, detached startup, blocked logs, and inherited signals: PASS\n'

"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/closed-stdio.c ${LDFLAGS:-} ${LDLIBS:-} -o "$work/closed-stdio"
"$work/closed-stdio" "$PWD/gu-run" --root "$work/closed" --rootfs "$work/rootfs" demo /bin/busybox echo closed-stdio
[ "$(./gu-logs --root "$work/closed" demo)" = closed-stdio ]
echo 'closed standard descriptors PASS'
