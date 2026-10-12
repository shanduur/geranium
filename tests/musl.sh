#!/bin/sh
set -eu
apk add --no-cache build-base binutils bmake busybox-static python3 openssl-dev openssl-libs-static zlib-dev zlib-static ca-certificates openssl
mkdir /tmp/geranium-build
cp -R /src/. /tmp/geranium-build/
cd /tmp/geranium-build
export GERANIUM_TEST_BUSYBOX=/bin/busybox.static
CFLAGS='-O2 -Wall -Wextra -Werror -pedantic' sh configure
bmake clean
bmake
# The core and command binaries really use the musl dynamic loader/libc.
readelf -h libgeranium.so | grep 'Type:.*DYN'
readelf -d libgeranium.so | grep '(SONAME).*\[libgeranium.so\]'
readelf -d libgeranium.so | grep '(NEEDED).*libc.musl-'
for v in create start run stop inspect logs rm pull rmi; do
    readelf -l "gu-$v" | grep 'interpreter: /lib/ld-musl-'
    readelf -d "gu-$v" | grep '(NEEDED).*\[libgeranium.so\]'
done
# Workload fixtures stay static, even though the manager now uses a DSO.
if readelf -l "$GERANIUM_TEST_BUSYBOX" | grep INTERP; then exit 1; fi
if readelf -d "$GERANIUM_TEST_BUSYBOX" | grep NEEDED; then exit 1; fi
# All selected tests (including installed consumer/relocation) run as both users.
# Root must exercise the actual TLS default port, not just remapped authorities.
GERANIUM_REQUIRE_DEFAULT_HOST=1 bmake test
sh tests/public-image.sh
bmake install DESTDIR=/tmp/geranium-install PREFIX='/opt/geranium space'
mv /tmp/geranium-install /tmp/geranium-moved
unset LD_LIBRARY_PATH
for v in create start run stop inspect logs rm pull rmi; do
    "/tmp/geranium-moved/opt/geranium space/bin/gu-$v" --help
done
adduser -D runner
chown -R runner:runner /tmp/geranium-build
su runner -c 'cd /tmp/geranium-build && GERANIUM_TEST_BUSYBOX=/bin/busybox.static bmake test && sh tests/public-image.sh'
su runner -c 'unset LD_LIBRARY_PATH; for v in create start run stop inspect logs rm pull rmi; do "/tmp/geranium-moved/opt/geranium space/bin/gu-$v" --help || exit; done'
