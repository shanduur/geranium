#!/bin/sh
set -eu
[ -x ./gu-create ] && [ -x ./gu-start ] || { echo 'separate entry points missing'; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
mkdir -p "$work/fs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-/usr/bin/busybox}" "$work/fs/bin/busybox"
./gu-run --root "$work/state" -d --rootfs "$work/fs" owned /bin/busybox sleep 30
hostns=$(readlink /proc/self/ns/pid)
found=0
for p in /proc/[0-9]*; do
 [ -r "$p/cmdline" ] || continue
 cmd=$(tr '\000' ' ' < "$p/cmdline" 2>/dev/null || :)
 case $cmd in *gu-*"$work/state"*)
 ns=$(readlink "$p/ns/pid" || :)
 [ "$ns" != "$hostns" ] || { echo "outside helper: $p $cmd"; exit 1; }
 found=$((found+1));;
 esac
done
[ "$found" -gt 0 ]
./gu-stop --root "$work/state" owned
./gu-rm --root "$work/state" owned
echo 'owned namespace processes PASS'
