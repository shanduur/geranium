#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
unset LD_LIBRARY_PATH DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH
library=$(sed -n 's/^LIBRARY = //p' Makefile)
library=${library:-libgeranium.so}
case $library in
libgeranium.so)
    consumer_runpath='-Wl,-rpath,$ORIGIN/../lib'
    if [ ! -f "$library" ]; then
        echo "shared-library contract: missing $library" >&2
        exit 1
    fi
    readelf -h libgeranium.so | grep 'Type:.*DYN' >/dev/null
    readelf -d libgeranium.so | grep '(SONAME).*\[libgeranium.so\]' >/dev/null
    readelf --dyn-syms -W libgeranium.so > "$work/exports"
    for verb in create start run stop inspect logs rm pull rmi; do
        readelf -l "gu-$verb" | grep INTERP >/dev/null
        readelf -d "gu-$verb" > "$work/dynamic"
        grep '(NEEDED).*\[libgeranium.so\]' "$work/dynamic" >/dev/null
        grep -E '\((RUNPATH|RPATH)\).*\[\$ORIGIN:\$ORIGIN/../lib\]' "$work/dynamic" >/dev/null
        # Reject every extra loader search directory (including build/staging paths).
        [ "$(awk '/\((RUNPATH|RPATH)\)/ { sub(/^.*\[/, ""); sub(/\].*$/, ""); print }' "$work/dynamic")" = '$ORIGIN:$ORIGIN/../lib' ]
        readelf --dyn-syms -W "gu-$verb" | grep "UND.* geranium_$verb$" >/dev/null
        awk -v name="geranium_$verb" '$4=="FUNC" && $5=="GLOBAL" && $7!="UND" && $8==name {found=1} END {exit !found}' "$work/exports"
        ./gu-"$verb" --help > "$work/help-$verb"
    done
    # Check actual ELF definitions, including local core implementations.
    nm --defined-only libgeranium.so | awk '$2 ~ /^[tT]$/ && $3 !~ /^_/ && $3 !~ /^(frame_dummy|register_tm_clones|deregister_tm_clones)$/ {print $3}' | sort -u > "$work/core"
    for verb in create start run stop inspect logs rm pull rmi; do
        nm --defined-only "gu-$verb" | awk '$2 ~ /^[tT]$/ {print $3}' | sort -u > "$work/command"
        comm -12 "$work/core" "$work/command" > "$work/overlap"
        test ! -s "$work/overlap"
    done
    awk '$4=="FUNC" && $5=="GLOBAL" && $7!="UND" {print $8}' "$work/exports" | sort > "$work/actual-api"
    { echo geranium_open_state_root; for verb in create start run stop inspect logs rm pull rmi; do echo "geranium_$verb"; done; } | sort > "$work/expected-api"
    cmp "$work/expected-api" "$work/actual-api"
    ;;
libgeranium.dylib)
    consumer_runpath='-Wl,-rpath,@loader_path/../lib'
    otool -D "$library" | grep -x '@rpath/libgeranium.dylib' >/dev/null
    nm -gUj "$library" | sort > "$work/actual-api"
    { echo _geranium_open_state_root; for verb in create start run stop inspect logs rm pull rmi; do echo "_geranium_$verb"; done; } | sort > "$work/expected-api"
    cmp "$work/expected-api" "$work/actual-api"
    nm -U "$library" | awk '$2 ~ /^[tT]$/ {print $3}' | sort -u > "$work/core"
    for verb in create start run stop inspect logs rm pull rmi; do
        otool -L "gu-$verb" | grep '@rpath/libgeranium.dylib ' >/dev/null
        otool -l "gu-$verb" | awk '$1=="cmd" {rpath=($2=="LC_RPATH")} rpath && $1=="path" {print $2}' > "$work/runpaths"
        printf '@loader_path\n@loader_path/../lib\n' > "$work/expected-runpaths"
        cmp "$work/expected-runpaths" "$work/runpaths"
        nm -uj "gu-$verb" | grep -x "_geranium_$verb" >/dev/null
        nm -U "gu-$verb" | awk '$2 ~ /^[tT]$/ {print $3}' | sort -u > "$work/command"
        comm -12 "$work/core" "$work/command" > "$work/overlap"
        test ! -s "$work/overlap"
        ./gu-"$verb" --help > "$work/help-$verb"
    done
    ;;
*) echo "unknown library format: $library" >&2; exit 1;;
esac
"${MAKE:-make}" install DESTDIR="$work/stage" PREFIX='/opt/geranium space'
mv "$work/stage" "$work/moved"
prefix="$work/moved/opt/geranium space"
test -f "$prefix/include/geranium.h"
# A separate translation unit consumes only the installed public header and DSO.
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2} -std=c99 -Wall -Wextra -Werror "-I$prefix/include" tests/library-consumer.c ${LDFLAGS:-} "-L$prefix/lib" "$consumer_runpath" -lgeranium ${LDLIBS:-} -o "$prefix/bin/consumer"
case $library in
libgeranium.so) readelf -d "$prefix/bin/consumer" | grep '(NEEDED).*\[libgeranium.so\]' >/dev/null;;
libgeranium.dylib) otool -L "$prefix/bin/consumer" | grep '@rpath/libgeranium.dylib ' >/dev/null;;
esac
for verb in create start run stop inspect logs rm pull rmi; do
    "$prefix/bin/gu-$verb" --help > "$work/installed"
    cmp "$work/help-$verb" "$work/installed"
done
mkdir "$work/fs"
"$prefix/bin/consumer" prepare-root "$work/state"
"$prefix/bin/consumer" create "$work/state" "$work/fs"
"$prefix/bin/consumer" inspect "$work/state" > "$work/inspect"
printf 'name=consumer\nstatus=created\n' > "$work/expected"
cmp "$work/expected" "$work/inspect"
"$prefix/bin/consumer" rm "$work/state"
test ! -e "$work/state/consumer"
for case_name in bad-enum null-root null-name bad-name negative-count oversize-count null-vector null-element null-source null-reference null-prepare-root; do
    if "$prefix/bin/consumer" "$case_name" "$work/negative-state" > "$work/out" 2> "$work/error"; then
        echo "direct API accepted $case_name" >&2; exit 1
    fi
    grep -F 'geranium:' "$work/error" >/dev/null
    test ! -e "$work/negative-state"
done
# libc interposition must still reach storage code inside the installed DSO.
if [ "$library" = libgeranium.so ]; then
    cat > "$work/preload.c" <<'EOF'
#include <dirent.h>
#include <errno.h>
struct dirent *readdir(DIR *dir) { (void)dir; errno=EIO; return 0; }
EOF
    "${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2} -fPIC -shared "$work/preload.c" ${LDFLAGS:-} ${LDLIBS:-} -o "$work/preload.so"
    if LD_PRELOAD="$work/preload.so" "$prefix/bin/gu-create" --root "$work/preload-state" --rootfs "$work/fs" fault /bin/true > "$work/out" 2> "$work/error"; then
        echo 'installed core ignored readdir EIO interposition' >&2; exit 1
    fi
    grep -E 'read source directory: (Input/output error|I/O error)' "$work/error" >/dev/null
    test ! -e "$work/preload-state/fault"
    echo 'installed DSO LD_PRELOAD readdir EIO, no publication PASS'
fi
# The moved install must require its DSO; build-tree copies cannot rescue it.
mv "$prefix/lib/$library" "$work/hidden-library"
for verb in create start run stop inspect logs rm pull rmi; do
    if "$prefix/bin/gu-$verb" --help > "$work/out" 2> "$work/error"; then
        echo "gu-$verb ran without its shared library" >&2; exit 1
    fi
    grep -F "$library" "$work/error" >/dev/null
    test ! -s "$work/out"
done
printf '%s: nine operations and root preparation, installed consumer, relocation and missing-library failure PASS\n' "$library"
