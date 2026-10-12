#!/bin/sh
set -eu
bin=./tests/cli.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir "$work/rootfs"
mkdir "$work/rejected-state"
chmod 0777 "$work/rejected-state"
if "$bin" --root "$work/rejected-state" create --rootfs "$work/rootfs" rejected /bin/true >"$work/rejected.log" 2>&1; then
    printf 'invalid state directory accepted\n' >&2; exit 1
fi
grep 'state directory must be owned by you and mode 0700' "$work/rejected.log" >/dev/null
[ ! -e "$work/rejected-state/.command-rejected.lock" ] || { printf 'invalid state directory created .command-rejected.lock\n' >&2; exit 1; }
[ -z "$(ls -A "$work/rejected-state")" ] || { printf 'invalid state directory mutated\n' >&2; exit 1; }
chmod 0700 "$work/rejected-state"
"$bin" --root "$work/rejected-state" create --rootfs "$work/rootfs" valid /bin/true
"$bin" --root "$work/rejected-state" inspect valid | grep 'status=created' >/dev/null
"$bin" --root "$work/rejected-state" rm valid
[ -f "$work/rejected-state/.command-valid.lock" ]
if "$bin" --root "$work/state" create --rootfs "$work/rootfs" invalid bin/command >/dev/null 2>&1; then
    printf 'relative command accepted\n' >&2; exit 1
fi
[ ! -e "$work/state/invalid" ] || { printf 'invalid container left behind\n' >&2; exit 1; }
printf 'absolute command validation PASS\n'
