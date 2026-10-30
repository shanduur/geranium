#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
reference=hello-world:latest
./gu-pull --root "$work/state" "$reference"
python3 tests/public-image-info.py "$work/state"
./gu-create --root "$work/state" --image "$reference" public
./gu-start --root "$work/state" public > "$work/first"
grep 'Hello from Docker!' "$work/first"
./gu-logs --root "$work/state" public > "$work/log"
cmp "$work/first" "$work/log"
./gu-rmi --root "$work/state" "$reference"
./gu-start --root "$work/state" public > "$work/restart"
cmp "$work/first" "$work/restart"
./gu-inspect --root "$work/state" public | grep status=exited
./gu-rm --root "$work/state" public
printf 'real public HTTPS pull/create/start/logs/restart/rmi/rm PASS\n'
