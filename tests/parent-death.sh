#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/parent-death.c src/backend_linux.c ${LDFLAGS:-} ${LDLIBS:-} -o "$work/parent-death"
"$work/parent-death"
printf 'bootstrap parent-death guard: PASS\n'
