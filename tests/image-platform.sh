#!/bin/sh
# Private DSO uses native Linux backend behavior and the actual production
# architecture dispatcher compiled with simulated target macros. No guest ABI
# or foreign kernel is executed by this test.
set -eu
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'private Linux image-platform integration SKIP (non-Linux host)'
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 -fPIC -Dbackend_image_arch=backend_native_image_arch -c src/backend_linux.c -o "$work/backend-native.o"
python3 tests/image-platform-fixture.py "$work"
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 -fPIC -c src/backend_unsupported.c -o "$work/unknown.o"
mkdir "$work/unknown"
"${CC:-cc}" ${CFLAGS--O2 -Wall -Wextra -Werror} ${LDFLAGS:-} -shared -Wl,-soname,libgeranium.so -Wl,--version-script,src/geranium.map -o "$work/unknown/libgeranium.so" geranium.o image.o registry.o json.o layer.o "$work/unknown.o" fatal.o ${LDLIBS:-} -lssl -lcrypto -lz -lyyjson -lskarnet
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -Wno-error=unused-function -std=c99 -o "$work/unknown/gu-pull" src/pull.c ${LDFLAGS:-} -L"$work/unknown" -Wl,-rpath,"$work/unknown" -lgeranium ${LDLIBS:-}
for arch in riscv64 ppc64le s390x; do
    mkdir "$work/$arch"
    "${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 -fPIC -c "$work/$arch.c" -o "$work/$arch/dispatcher.o"
    "${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -std=c99 -o "$work/$arch/probe" tests/image-platform-backend.c "$work/$arch/dispatcher.o" "$work/backend-native.o"
    "${CC:-cc}" ${CFLAGS--O2 -Wall -Wextra -Werror} ${LDFLAGS:-} -shared -Wl,-soname,libgeranium.so -Wl,--version-script,src/geranium.map -o "$work/$arch/libgeranium.so" geranium.o image.o registry.o json.o layer.o "$work/backend-native.o" "$work/$arch/dispatcher.o" fatal.o ${LDLIBS:-} -lssl -lcrypto -lz -lyyjson -lskarnet
    for name in pull create; do
        "${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS--O2 -Wall -Wextra -Werror} -Wno-error=unused-function -std=c99 -o "$work/$arch/gu-$name" "src/$name.c" ${LDFLAGS:-} -L"$work/$arch" -Wl,-rpath,"$work/$arch" -lgeranium ${LDLIBS:-}
    done
    if [ "$arch" = riscv64 ]; then
        python3 tests/image-platform.py "$work/$arch/gu-pull" "$work/$arch/gu-create" "$work/$arch/probe" "$arch" "$work/unknown/gu-pull"
    else
        python3 tests/image-platform.py "$work/$arch/gu-pull" "$work/$arch/gu-create" "$work/$arch/probe" "$arch"
    fi
done
