#!/bin/sh
# Linux/ELF-only helper for the namespace/ptrace fault suite.
# Link fault instrumentation into a private DSO from the production PIC objects.
# The command owns only CLI parsing/main and calls its typed library operation.
set -eu
output=$1; verb=$2; fixture=$3; shift 3
name=$(basename "$output")
libdir=$(dirname "$output")/.lib-"$name"
mkdir -p "$libdir"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -fPIC -std=c99 "$fixture" geranium.o image.o registry.o json.o layer.o backend.o ${LDFLAGS:-} -shared -Wl,-soname,libgeranium.so -Wl,--version-script,src/geranium.map "$@" ${LDLIBS:-} -lssl -lcrypto -lz -lyyjson -lskarnet -o "$libdir/libgeranium.so"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 "src/$verb.c" ${LDFLAGS:-} "-L$libdir" -Wl,-rpath,'$ORIGIN'"/.lib-$name" -lgeranium ${LDLIBS:-} -o "$output"
