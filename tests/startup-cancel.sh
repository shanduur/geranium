#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$w/fs/bin/busybox"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/startup-cancel.c ${LDFLAGS:-} ${LDLIBS:-} -lssl -lcrypto -lz -o "$w/check"
sh tests/fault-command.sh "$w/run" run tests/preexec-stall.c -Wl,--wrap=setpgid
for mode in foreground detached timeout; do
    "$w/check" "$w/run" "$w/state" "$w/fs" "$mode"
    ./gu-rm --root "$w/state" "$mode"
done
# The recorded eight-second finite delay must also cancel before it expires.
sh tests/fault-command.sh "$w/run" run tests/preexec-stall.c -DPREEXEC_DELAY_MS=8000 -Wl,--wrap=setpgid
for mode in foreground detached; do
    "$w/check" "$w/run" "$w/state" "$w/fs" "$mode"
    ./gu-rm --root "$w/state" "$mode"
done
