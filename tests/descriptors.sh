#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/bin"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -static tests/fd-fixture.c ${LDFLAGS:-} ${LDLIBS:-} -o "$w/fs/bin/check"
set +e
"$w/fs/bin/check" "${GERANIUM_RUN:-./gu-run}" --root "$w/state" --rootfs "$w/fs" fd /bin/check check > "$w/out"
rc=$?
set -e
cat "$w/out"
[ "$rc" = 0 ]
[ "$(cat "$w/out")" = 'descriptors isolated' ]
echo 'high inherited descriptor isolation PASS'
