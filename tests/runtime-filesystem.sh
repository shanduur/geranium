#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 \
    tests/runtime-filesystem.c src/backend_linux.c ${LDFLAGS:-} ${LDLIBS:-} \
    -o "$work/runtime-filesystem"
mkdir "$work/root"
"$work/runtime-filesystem" "$work/root"
test -d "$work/root/proc"
test -d "$work/root/dev"
test ! -e "$work/root/dev/null"
test ! -e "$work/root/proc/self/stat"
mkdir "$work/bad"
ln -s /tmp "$work/bad/proc"
if "$work/runtime-filesystem" "$work/bad" >"$work/out" 2>"$work/error"; then
    printf 'accepted image-controlled /proc symlink\n' >&2
    exit 1
fi
if ! grep -F 'proc: Not a directory' "$work/error" >/dev/null; then
    printf 'unexpected symlink diagnostic\n' >&2
    exit 1
fi
printf 'private runtime proc, dev, pts, shm and tmp PASS\n'
