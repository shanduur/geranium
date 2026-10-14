#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 tests/registry-deadline.c src/json.c src/fatal.c ${LDFLAGS:-} ${LDLIBS:-} -lssl -lcrypto -lyyjson -o "$work/deadline"
status=0
diagnostic=$("$work/deadline" 2>&1) || status=$?
if [ "$status" -ne 1 ]; then printf '%s\n' "$diagnostic" 'hard deadline was not enforced'; exit 1; fi
case "$diagnostic" in *'request deadline'*) ;; *) printf '%s\n' "$diagnostic"; exit 1;; esac
case "$diagnostic" in *'positive I/O progress'*) ;; *) printf '%s\n' "$diagnostic"; exit 1;; esac
printf '%s\n' 'body progress cannot extend the hard deadline PASS'
python3 tests/image-selectors.py
python3 tests/required-default-host.py
for behavior in $(python3 tests/images.py --list); do
    python3 tests/images.py "$behavior"
done
