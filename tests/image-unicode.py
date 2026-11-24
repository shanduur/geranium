#!/usr/bin/env python3
"""Image paths must not depend on the invoking user's locale."""

import io
import os
import tarfile
import tempfile

from images import Registry, call, published_roots


def main():
    os.environ["LC_ALL"] = "C"
    for format, name in (
        (tarfile.PAX_FORMAT, "etc/certs/Főtanúsítvány.crt"),
        (tarfile.GNU_FORMAT, "etc/certs/Főtanúsítvány.crt"),
        (tarfile.USTAR_FORMAT, "etc/certs/\udcff.crt"),
    ):
        data = b"locale-independent OCI filename\n"
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode="w", format=format) as archive:
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
            for link_type, link_name in (
                (tarfile.SYMTYPE, "symlink"),
                (tarfile.LNKTYPE, "hardlink"),
            ):
                link = tarfile.TarInfo(link_name)
                link.type = link_type
                link.linkname = name
                archive.addfile(link)
        with (
            tempfile.TemporaryDirectory() as state,
            Registry([output.getvalue()]) as registry,
        ):
            call("pull", "--root", state, "--http-loopback", registry.ref)
            roots = published_roots(state)
            assert len(roots) == 1
            assert (roots[0] / name).read_bytes() == data
            assert os.readlink(roots[0] / "symlink") == name
            assert (roots[0] / "symlink").read_bytes() == data
            assert (roots[0] / "hardlink").read_bytes() == data
            assert (roots[0] / "hardlink").stat().st_ino == (
                roots[0] / name
            ).stat().st_ino
    print("PAX/GNU UTF-8 and USTAR byte filenames and links preserved in C locale PASS")


if __name__ == "__main__":
    main()
