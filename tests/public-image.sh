#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
reference=hello-world:latest
./gu-pull --root "$work/state" "$reference"
python3 - "$work/state" <<'PY'
import sys,pathlib,json,hashlib
p=next(pathlib.Path(sys.argv[1]).glob('.images/*/manifest.json'))
print('Public Docker Hub selected manifest: sha256:'+hashlib.sha256(p.read_bytes()).hexdigest())
c=json.loads(p.parent.joinpath('config.json').read_bytes())
print('Public image platform:',c['os'],c['architecture'],'command:',c['config']['Cmd'])
PY
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
