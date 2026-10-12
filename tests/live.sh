#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
child=
trap 'if [ -n "$child" ]; then kill "$child" 2>/dev/null || :; fi; rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/rootfs/bin"
cp "${GERANIUM_TEST_BUSYBOX:-$(command -v busybox)}" "$work/rootfs/bin/busybox"
"$bin" --root "$work/state" create --rootfs "$work/rootfs" slow /bin/busybox sleep 5
"$bin" --root "$work/state" start slow > "$work/out" 2>&1 & child=$!
tries=0
until timeout 1 "$bin" --root "$work/state" inspect slow | grep 'status=running' >/dev/null; do
    tries=$((tries+1)); [ "$tries" -lt 100 ] || { echo 'never running' >&2; exit 1; }
    sleep .05
done
if ! timeout 2 "$bin" --root "$work/state" inspect slow | grep 'status=running' >/dev/null; then echo 'inspect blocked during start' >&2; exit 1; fi
if ! timeout 2 "$bin" --root "$work/state" logs slow >/dev/null; then echo 'logs blocked during start' >&2; exit 1; fi
kill -KILL "$child"; wait "$child" 2>/dev/null || :; child=
tries=0
until "$bin" --root "$work/state" inspect slow | grep 'status=exited' >/dev/null; do
    tries=$((tries+1)); [ "$tries" -lt 100 ] || { echo 'dead client left workload running' >&2; exit 1; }
    sleep .05
done
printf 'live lifecycle PASS\n'
