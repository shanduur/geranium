#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/publication.c src/image.c src/registry.c src/json.c src/layer.c src/backend_unsupported.c ${LDFLAGS:-} ${LDLIBS:-} -lssl -lcrypto -lz -lyyjson -lskarnet -o "$work/publication"
"$work/publication"
printf 'publication race: PASS\n'
