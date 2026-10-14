#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/json-contract.c src/json.c src/fatal.c ${LDFLAGS:-} ${LDLIBS:-} -lyyjson -o "$work/json"
python3 tests/json-contract.py "$work/json"
