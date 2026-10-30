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
python3 - "$work" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
source = pathlib.Path('src/backend_linux.c').read_text()
match = re.search(r'const char \*backend_image_arch\(void\)\n\{.*?\n\}', source, re.S)
assert match, 'production architecture dispatcher not found'
macros = ('__x86_64__', '__aarch64__', '__riscv', '__riscv_xlen',
          '__powerpc64__', '__s390x__', '__s390__', '__BYTE_ORDER__')
cases = {
    'riscv64': {'__riscv': '1', '__riscv_xlen': '64'},
    'ppc64le': {'__powerpc64__': '1', '__BYTE_ORDER__': '__ORDER_LITTLE_ENDIAN__'},
    's390x': {'__s390x__': '1'},
}
for arch, definitions in cases.items():
    text = '#include <stddef.h>\n'
    text += ''.join('#undef ' + macro + '\n' for macro in macros)
    text += ''.join('#define ' + key + ' ' + value + '\n' for key, value in definitions.items())
    (root / (arch + '.c')).write_text(text + match.group(0) + '\n')
PY
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
