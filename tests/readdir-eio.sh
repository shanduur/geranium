#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
mkdir -p "$w/fs/sub"
printf 'first\n' > "$w/fs/first"
printf 'second\n' > "$w/fs/second"
printf 'nested\n' > "$w/fs/sub/nested"
printf 'another\n' > "$w/fs/sub/another"
ln -s sub/nested "$w/fs/link"
sh tests/fault-command.sh "$w/create" create tests/readdir-eio.c -Wl,--wrap=readdir
if GERANIUM_READDIR_EIO="$w/fs" "$w/create" --root "$w/state" --rootfs "$w/fs" copy /first > "$w/out" 2> "$w/err"; then
    printf 'create accepted a source readdir EIO\n' >&2
    exit 1
fi
grep -q 'read source directory:' "$w/err"
test ! -e "$w/state/copy"
test -d "$w/state/.pending-copy"
# Cleanup enumeration must also fail without publishing or hiding the error.
if GERANIUM_READDIR_EIO="$w/state/.pending-copy/rootfs" "$w/create" --root "$w/state" --rootfs "$w/fs" copy /first > "$w/out" 2> "$w/err"; then
    printf 'create accepted a cleanup readdir EIO\n' >&2
    exit 1
fi
grep -q 'read removable directory:' "$w/err"
test ! -e "$w/state/copy"
# Retry removes the partial stage and must preserve every entry.
"$w/create" --root "$w/state" --rootfs "$w/fs" copy /first
test ! -e "$w/state/.pending-copy"
diff -r "$w/fs" "$w/state/copy/rootfs"
./gu-inspect --root "$w/state" copy > "$w/inspect"
grep -qx 'status=created' "$w/inspect"
# A failure below the source root must also prevent publication.
if GERANIUM_READDIR_EIO="$w/fs/sub" "$w/create" --root "$w/state" --rootfs "$w/fs" nested /first > "$w/out" 2> "$w/err"; then
    printf 'create accepted a nested readdir EIO\n' >&2
    exit 1
fi
grep -q 'read source directory:' "$w/err"
test ! -e "$w/state/nested"
"$w/create" --root "$w/state" --rootfs "$w/fs" nested /first
diff -r "$w/fs" "$w/state/nested/rootfs"
printf 'readdir EIO publication and retry: PASS\n'
