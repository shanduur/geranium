import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
source = pathlib.Path("src/backend_linux.c").read_text()
match = re.search(r"const char \*backend_image_arch\(void\)\n\{.*?\n\}", source, re.S)
assert match, "production architecture dispatcher not found"
macros = (
    "__x86_64__",
    "__aarch64__",
    "__riscv",
    "__riscv_xlen",
    "__powerpc64__",
    "__s390x__",
    "__s390__",
    "__BYTE_ORDER__",
)
cases = {
    "riscv64": {"__riscv": "1", "__riscv_xlen": "64"},
    "ppc64le": {"__powerpc64__": "1", "__BYTE_ORDER__": "__ORDER_LITTLE_ENDIAN__"},
    "s390x": {"__s390x__": "1"},
}
for arch, definitions in cases.items():
    text = "#include <stddef.h>\n"
    text += "".join("#undef " + macro + "\n" for macro in macros)
    text += "".join(
        "#define " + key + " " + value + "\n" for key, value in definitions.items()
    )
    (root / (arch + ".c")).write_text(text + match.group(0) + "\n")
