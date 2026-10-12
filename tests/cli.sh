#!/bin/sh
set -eu
root=$2; verb=$3; shift 3
exec "${GERANIUM_BINDIR:-.}/gu-$verb" --root "$root" "$@"
