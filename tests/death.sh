#!/bin/sh
set -eu
w=$(mktemp -d)
client=
trap 'if [ -n "$client" ]; then kill -KILL "$client" 2>/dev/null || :; fi; rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$w/fs/bin/busybox"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -static tests/fd-fixture.c ${LDFLAGS:-} ${LDLIBS:-} -o "$w/helper"
./gu-run --root "$w/state" --rootfs "$w/fs" dead /bin/busybox sleep 30 > "$w/out" & client=$!
i=0
until ./gu-inspect --root "$w/state" dead 2>/dev/null | grep status=running >/dev/null; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .05; done
"$w/helper" kill-init "$w/state/dead/runtime"
i=0
while kill -0 "$client" 2>/dev/null; do
 i=$((i+1)); [ "$i" -lt 40 ] || { echo 'foreground client hangs after external init death'; exit 1; }; sleep .05
done
set +e
wait "$client"
rc=$?
set -e
client=
[ "$rc" = 125 ]
./gu-inspect --root "$w/state" dead | grep status=exited
[ ! -e "$w/state/dead/exitcode" ]
./gu-rm --root "$w/state" dead
echo 'external init death recovery PASS'
