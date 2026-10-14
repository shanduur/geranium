#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
# Private shared core DSO: production PIC objects; existing fault wrappers
# are inert without a fault selection. Only CPU probes force two workers.
sh tests/fault-command.sh "$work/pull" pull tests/pull-workers.c \
    -Wl,--wrap=fork -Wl,--wrap=fsync -Wl,--wrap=mmap -Wl,--wrap=openat \
    -Wl,--wrap=sched_getaffinity -Wl,--wrap=sysconf \
    -Wl,--wrap=backend_watch_parent -Wl,--wrap=atexit
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 \
    tests/pull-fatal-caller.c ${LDFLAGS:-} -L"$work/.lib-pull" \
    -Wl,-rpath,"$work/.lib-pull" -lgeranium ${LDLIBS:-} -o "$work/caller"
python3 tests/pull-fatal.py "$work/caller"
