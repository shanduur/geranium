#!/usr/bin/env python3
"""Focused real layer_apply fixture, independent of the registry and namespaces."""

import ctypes
import ctypes.util
import gzip
import hashlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile

from images import OCI, Registry, call, digest, encode, published_roots

BASE = pathlib.Path(__file__).resolve().parent.parent


def zstd_compress(data):
    zstd = ctypes.CDLL(ctypes.util.find_library("zstd"))
    zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
    zstd.ZSTD_compressBound.restype = ctypes.c_size_t
    zstd.ZSTD_compress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
    ]
    zstd.ZSTD_compress.restype = ctypes.c_size_t
    zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
    source = ctypes.create_string_buffer(data)
    bound = zstd.ZSTD_compressBound(len(data))
    output = ctypes.create_string_buffer(bound)
    used = zstd.ZSTD_compress(output, bound, source, len(data), 1)
    assert not zstd.ZSTD_isError(used)
    return output.raw[:used]


def fixture(entries, format=tarfile.PAX_FORMAT):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=format) as archive:
        for entry in entries:
            archive.addfile(entry[0], io.BytesIO(entry[1]) if entry[1] else None)
    return output.getvalue()


def file(name, data, uid=0, gid=0, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.uid, info.gid, info.mode = uid, gid, mode
    return info, data


def worker(libpath, rootpath, stagepath, layerpath, compression, rawpath):
    library = ctypes.CDLL(libpath)
    library.layer_apply.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_ulonglong),
    ]
    data = pathlib.Path(layerpath).read_bytes()
    diff = (
        b"sha256:"
        + hashlib.sha256(pathlib.Path(rawpath).read_bytes()).hexdigest().encode()
    )
    root = os.open(rootpath, os.O_RDONLY | os.O_DIRECTORY)
    stage = os.open(stagepath, os.O_RDONLY | os.O_DIRECTORY)
    expanded = ctypes.c_ulonglong(0)
    library.layer_apply(
        root, stage, data, len(data), compression, diff, ctypes.byref(expanded)
    )
    os.close(root)
    os.close(stage)


def main():
    with tempfile.TemporaryDirectory() as work:
        tmp = pathlib.Path(work)
        lib = tmp / "liblayer.so"
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-o",
                str(lib),
                str(BASE / "src/layer.c"),
                str(BASE / "src/fatal.c"),
                "-lcrypto",
                "-lz",
                "-larchive",
                "-lzstd",
            ],
            check=True,
        )
        root, stage = tmp / "root", tmp / "stage"
        root.mkdir()
        stage.mkdir()
        data = b"non-root OCI fixture\n"
        layer = tmp / "fixture.tar"
        layer.write_bytes(fixture([file("etc/owner", data, 1000, 1001, 0o640)]))
        result = subprocess.run(
            [
                sys.executable,
                __file__,
                "--worker",
                str(lib),
                str(root),
                str(stage),
                str(layer),
                "0",
                str(layer),
            ],
            capture_output=True,
        )
        assert result.returncode == 0, result.stderr
        destination = root / "etc/owner"
        assert destination.read_bytes() == data
        assert destination.stat().st_mode & 0o7777 == 0o640
        assert (destination.stat().st_uid, destination.stat().st_gid) == (
            os.geteuid(),
            os.getegid(),
        )
        assert result.stderr.count(b"ownership") == 1, result.stderr
        print("PASS: non-root ownership flattened, bytes and permissions retained")

        for compression, encoded in (
            (1, gzip.compress(layer.read_bytes())),
            (2, zstd_compress(layer.read_bytes())),
        ):
            encoded_path = tmp / f"encoded-{compression}"
            encoded_path.write_bytes(encoded)
            compressed_root = tmp / f"root-{compression}"
            compressed_stage = tmp / f"stage-{compression}"
            compressed_root.mkdir()
            compressed_stage.mkdir()
            result = subprocess.run(
                [
                    sys.executable,
                    __file__,
                    "--worker",
                    str(lib),
                    str(compressed_root),
                    str(compressed_stage),
                    str(encoded_path),
                    str(compression),
                    str(layer),
                ],
                capture_output=True,
            )
            assert result.returncode == 0, result.stderr
            assert (compressed_root / "etc/owner").read_bytes() == data
            print(f"PASS: compression {compression} verifies DiffID and extracts")

    raw = fixture([file("owner", b"codec fixture\n", 1000, 1001)])
    for media, encoded in (
        (OCI + "layer.v1.tar", raw),
        (OCI + "layer.v1.tar+zstd", zstd_compress(raw)),
        (OCI + "layer.nondistributable.v1.tar+zstd", zstd_compress(raw)),
    ):
        with tempfile.TemporaryDirectory() as state, Registry([raw]) as registry:
            document = json.loads(registry.manifest)
            encoded_digest = digest(encoded)
            registry.blobs[encoded_digest] = encoded
            document["layers"][0].update(
                mediaType=media, digest=encoded_digest, size=len(encoded)
            )
            registry.manifest = encode(document)
            call("pull", "--root", state, "--http-loopback", registry.ref)
            assert (
                published_roots(state)[0] / "owner"
            ).read_bytes() == b"codec fixture\n"
    print("PASS: raw and Zstandard descriptors through the actual registry CLI")


if __name__ == "__main__":
    if len(sys.argv) == 8 and sys.argv[1] == "--worker":
        worker(
            sys.argv[2],
            sys.argv[3],
            sys.argv[4],
            sys.argv[5],
            int(sys.argv[6]),
            sys.argv[7],
        )
    else:
        main()
