#!/bin/sh
set -eu
python3 - <<'PY'
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

source = pathlib.Path('src/backend_linux.c').read_text()
match = re.search(r'const char \*backend_image_arch\(void\)\n\{.*?\n\}', source, re.S)
assert match, 'production architecture dispatcher not found'
macros = ('__x86_64__', '__aarch64__', '__riscv', '__riscv_xlen',
          '__powerpc64__', '__s390x__', '__s390__', '__BYTE_ORDER__')
cases = (
    ('riscv64', {'__riscv': '1', '__riscv_xlen': '64'}, 'riscv64'),
    ('ppc64le', {'__powerpc64__': '1', '__BYTE_ORDER__': '__ORDER_LITTLE_ENDIAN__'}, 'ppc64le'),
    ('s390x', {'__s390x__': '1'}, 's390x'),
    ('amd64', {'__x86_64__': '1'}, 'amd64'),
    ('arm64', {'__aarch64__': '1'}, 'arm64'),
    ('riscv32', {'__riscv': '1', '__riscv_xlen': '32'}, ''),
    ('ppc64-big-endian', {'__powerpc64__': '1', '__BYTE_ORDER__': '__ORDER_BIG_ENDIAN__'}, ''),
    ('s390-31-bit', {'__s390__': '1'}, ''),
    ('unknown', {}, ''),
)
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    for label, definitions, expected in cases:
        text = '#include <string.h>\n'
        text += ''.join('#undef ' + macro + '\n' for macro in macros)
        text += ''.join('#define ' + key + ' ' + value + '\n' for key, value in definitions.items())
        text += match.group(0)
        text += '\nint main(int argc, char **argv)\n{\n'
        text += '\tconst char *arch = backend_image_arch();\n'
        text += '\treturn argc != 2 || (arch ? strcmp(arch, argv[1]) : *argv[1] != 0);\n}\n'
        c = root / (label + '.c')
        binary = root / label
        c.write_text(text)
        command = [os.environ.get('CC', 'cc')]
        command += shlex.split(os.environ.get('CPPFLAGS', ''))
        command += shlex.split(os.environ.get('CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic'))
        command += ['-std=c99', str(c)]
        command += shlex.split(os.environ.get('LDFLAGS', ''))
        command += shlex.split(os.environ.get('LDLIBS', ''))
        command += ['-o', str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary), expected])
        assert result.returncode == 0, 'production architecture mapping failed: ' + label
        print('production architecture mapping ' + label + ' PASS', flush=True)
PY
