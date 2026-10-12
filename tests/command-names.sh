#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
for verb in create start run stop inspect logs rm pull rmi; do
    program="./gu-$verb"
    test -x "$program" || { printf 'missing command: %s\n' "$program" >&2; exit 1; }
    "$program" --help > "$work/help"
    grep "^usage: gu-$verb " "$work/help"
    test ! -e "./geranium-$verb"
done
# CLI token validation owns syntax diagnostics; the typed library must still
# validate names independently for non-CLI callers.
long_name=$(printf '%064d' 0)
for verb in start stop inspect logs rm create run; do
    case "$verb" in
        start) diagnostic='start [-d] NAME' ;;
        stop) diagnostic='stop NAME' ;;
        logs) diagnostic='logs NAME' ;;
        inspect|rm) diagnostic='inspect|rm NAME' ;;
        create|run) diagnostic='invalid container name' ;;
    esac
    printf 'geranium: %s\n' "$diagnostic" > "$work/expected"
    for name in '../bad' '-bad' 'bad.name' '' "$long_name"; do
        case "$verb" in
            create|run) set -- --rootfs "$work/rootfs" "$name" /bin/true ;;
            *) set -- "$name" ;;
        esac
        if "./gu-$verb" --root "$work/state" "$@" > "$work/out" 2> "$work/error"; then
            printf '%s accepted malformed name\n' "$verb" >&2
            exit 1
        fi
        test ! -s "$work/out"
        cmp "$work/expected" "$work/error"
    done
done
mkdir "$work/rootfs"
./gu-create --root "$work/state" --rootfs "$work/rootfs" _valid-0 /bin/true
./gu-inspect --root "$work/state" _valid-0 > "$work/inspect"
./gu-rm --root "$work/state" _valid-0
printf 'geranium: too many arguments\n' > "$work/expected"
set -- /bin/true
count=0
while test "$count" -lt 64; do
    set -- "$@" arg
    count=$((count + 1))
done
for verb in create run; do
    if "./gu-$verb" --root "$work/state" --rootfs "$work/rootfs" limit "$@" > "$work/out" 2> "$work/error"; then
        printf '%s accepted too many workload arguments\n' "$verb" >&2
        exit 1
    fi
    cmp "$work/expected" "$work/error"
    test ! -e "$work/state/limit"
done
printf 'CLI name grammar, argument limits and syntax diagnostics PASS\n'
# Cross invalid roots with malformed operation arguments. Root failures precede
# syntax failures; valid roots are prepared even when operation syntax is bad.
mkdir "$work/unsafe"
chmod 0755 "$work/unsafe"
for root in /dev/null relative "$work/unsafe"; do
    if ./gu-inspect --root "$root" _valid-0 > "$work/out" 2> "$work/root-error"; then
        printf 'accepted invalid root\n' >&2; exit 1
    fi
    for verb in create start run stop inspect logs rm pull rmi; do
        case "$verb" in pull|rmi) set -- ../bad extra ;; *) set -- ../bad ;; esac
        if "./gu-$verb" --root "$root" "$@" > "$work/out" 2> "$work/error"; then
            printf '%s accepted malformed operation\n' "$verb" >&2; exit 1
        fi
        cmp "$work/root-error" "$work/error"
        test ! -s "$work/out"
    done
done
for verb in create start run stop inspect logs rm pull rmi; do
    root="$work/prepared-$verb"
    case "$verb" in pull|rmi) set -- ../bad extra ;; *) set -- ../bad ;; esac
    if "./gu-$verb" --root "$root" "$@" > "$work/out" 2> "$work/error"; then
        printf '%s accepted malformed operation\n' "$verb" >&2; exit 1
    fi
    test -d "$root"
done
if ./gu-inspect --root "$work/missing-operation" > "$work/out" 2> "$work/error"; then
    printf 'accepted missing operation\n' >&2; exit 1
fi
test ! -e "$work/missing-operation"
printf 'CLI root preparation and error precedence PASS\n'
make install DESTDIR="$work/install" PREFIX=/opt/geranium
for verb in create start run stop inspect logs rm pull rmi; do
    test -x "$work/install/opt/geranium/bin/gu-$verb"
    test ! -e "$work/install/opt/geranium/bin/geranium-$verb"
done
printf 'gu command names, usage, installation, and absence of legacy aliases PASS\n'
