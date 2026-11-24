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
import resource
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


def tar_end_regression():
    raw = fixture([file("kept", b"one")])
    end = raw.index(bytes(1024), 1024)
    zero_data = bytes(1024)
    zero_raw = fixture([file("zero-data", zero_data)], format=tarfile.USTAR_FORMAT)
    zero_end = 512 + len(zero_data)  # USTAR header and aligned file payload.
    assert zero_raw[zero_end : zero_end + 1024] == bytes(1024)
    cases = {
        "missing": raw[:end],
        "single-zero": raw[: end + 512],
        "nonzero-padding": raw[: end + 1024] + b"x" + bytes(511),
        "early-zero": raw[: end + 512] + b"x" + bytes(511) + raw[end + 1024 :],
        "zero-data-missing": zero_raw[:zero_end],
        "zero-data-single-zero": zero_raw[: zero_end + 512],
        "zero-data-dirty-trailer": zero_raw[: zero_end + 1024] + b"x" + bytes(511),
    }
    for name, malformed in cases.items():
        for media, encode_layer in (
            (OCI + "layer.v1.tar", lambda data: data),
            (OCI + "layer.v1.tar+gzip", gzip.compress),
            (OCI + "layer.v1.tar+zstd", zstd_compress),
        ):
            encoded = encode_layer(malformed)
            with (
                tempfile.TemporaryDirectory() as state,
                Registry([malformed]) as registry,
            ):
                document = json.loads(registry.manifest)
                identity = digest(encoded)
                registry.blobs[identity] = encoded
                document["layers"][0].update(
                    mediaType=media, digest=identity, size=len(encoded)
                )
                registry.manifest = encode(document)
                rejected = call(
                    "pull", "--root", state, "--http-loopback", registry.ref, ok=False
                )
                assert b"invalid tar terminator" in rejected.stderr, (
                    name,
                    media,
                    rejected.stderr,
                )
                assert not published_roots(state), (name, media)
    with tempfile.TemporaryDirectory() as state, Registry([raw]) as registry:
        call("pull", "--root", state, "--http-loopback", registry.ref)
        assert (published_roots(state)[0] / "kept").read_bytes() == b"one"
    for media, encode_layer in (
        (OCI + "layer.v1.tar", lambda data: data),
        (OCI + "layer.v1.tar+gzip", gzip.compress),
        (OCI + "layer.v1.tar+zstd", zstd_compress),
    ):
        encoded = encode_layer(zero_raw)
        with tempfile.TemporaryDirectory() as state, Registry([zero_raw]) as registry:
            document = json.loads(registry.manifest)
            identity = digest(encoded)
            registry.blobs[identity] = encoded
            document["layers"][0].update(
                mediaType=media, digest=identity, size=len(encoded)
            )
            registry.manifest = encode(document)
            call("pull", "--root", state, "--http-loopback", registry.ref)
            assert (published_roots(state)[0] / "zero-data").read_bytes() == zero_data
    print(
        "PASS: raw/gzip/zstd strict tar ends reject without publication; standard and zero-filled tar pass"
    )


def special_replacement_regression():
    link = tarfile.TarInfo("symlink")
    link.type = tarfile.SYMTYPE
    link.linkname = "regular"
    lower = fixture(
        [
            file("regular", b"old regular"),
            (link, b""),
            file("directory/child", b"old child"),
        ]
    )
    special = []
    for name, kind in (
        ("regular", tarfile.FIFOTYPE),
        ("symlink", tarfile.CHRTYPE),
        ("directory", tarfile.FIFOTYPE),
    ):
        entry = tarfile.TarInfo(name)
        entry.type = kind
        special.append((entry, b""))
    with (
        tempfile.TemporaryDirectory() as state,
        Registry([lower, fixture(special)]) as registry,
    ):
        result = call("pull", "--root", state, "--http-loopback", registry.ref)
        root = published_roots(state)[0]
        for name in ("regular", "symlink", "directory"):
            assert not (root / name).is_symlink(), name
            assert not (root / name).exists(), name
        assert result.stderr.count(b"unsupported device/FIFO/socket") == 1, (
            result.stderr
        )
    print(
        "PASS: FIFO/device replacement removes regular, symlink and directory; no special node created"
    )


def worker(libpath, rootpath, stagepath, layerpath, compression, rawpath, preseed=0):
    library = ctypes.CDLL(libpath)
    library.layer_apply.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_ulonglong),
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
    output_total = ctypes.c_ulonglong(preseed)
    if preseed:
        resource.setrlimit(resource.RLIMIT_FSIZE, (2 * 1024 * 1024, 2 * 1024 * 1024))
    library.layer_apply(
        root,
        stage,
        data,
        len(data),
        compression,
        diff,
        ctypes.byref(expanded),
        ctypes.byref(output_total),
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

        if len(sys.argv) == 2 and sys.argv[1] == "--fd":
            layers = [fixture([file(f"files/{i}", b"x")]) for i in range(48)]
            with tempfile.TemporaryDirectory() as state, Registry(layers) as registry:

                def limit_fds():
                    resource.setrlimit(resource.RLIMIT_NOFILE, (96, 96))

                pulled = subprocess.run(
                    [
                        str(BASE / "gu-pull"),
                        "--root",
                        state,
                        "--http-loopback",
                        registry.ref,
                    ],
                    capture_output=True,
                    preexec_fn=limit_fds,
                )
                assert pulled.returncode == 0, pulled.stderr
                assert (published_roots(state)[0] / "files/47").read_bytes() == b"x"
            print("PASS: 48-layer real pull under 96-descriptor limit")
            return

        if len(sys.argv) == 2 and sys.argv[1] == "--sparse":
            sparse = tmp / "sparse.tar"
            sparse_entry, sparse_data = file("hole", b"\0\0")
            sparse_entry.pax_headers = {
                "GNU.sparse.map": "0,1,33554431,1",
                "GNU.sparse.size": str(32 * 1024 * 1024),
            }
            sparse.write_bytes(fixture([(sparse_entry, sparse_data)]))
            sparse_root, sparse_stage = tmp / "sparse-root", tmp / "sparse-stage"
            sparse_root.mkdir()
            sparse_stage.mkdir()
            result = subprocess.run(
                [
                    sys.executable,
                    __file__,
                    "--worker",
                    str(lib),
                    str(sparse_root),
                    str(sparse_stage),
                    str(sparse),
                    "0",
                    str(sparse),
                    str(4 * 1024**3 - 1024**2),
                ],
                capture_output=True,
            )
            assert (
                result.returncode != 0 and b"extracted image exceeds" in result.stderr
            ), result.stderr
            assert not (sparse_root / "hole").exists()
            print("PASS: sparse logical output rejected before writing holes")
            return

        if len(sys.argv) == 2 and sys.argv[1] == "--pax":
            pax_entry, pax_data = file("fallback", b"should not publish")
            pax_entry.pax_headers = {"path": "valid-pax-name"}
            malformed = bytearray(fixture([(pax_entry, pax_data)]))
            marker = malformed.index(b" path=", 512)
            assert 512 < marker < 1024
            malformed[512] = ord("0")
            with (
                tempfile.TemporaryDirectory() as state,
                Registry([bytes(malformed)]) as registry,
            ):
                rejected = call(
                    "pull", "--root", state, "--http-loopback", registry.ref, ok=False
                )
                assert b"corrupt tar header" in rejected.stderr, rejected.stderr
                assert not published_roots(state)
            print("PASS: malformed PAX record rejected without publication")
            return

        if len(sys.argv) == 2 and sys.argv[1] == "--terminator":
            tar_end_regression()
            return

        if len(sys.argv) == 2 and sys.argv[1] == "--special":
            special_replacement_regression()
            return

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

    for regression in ("fd", "sparse", "pax", "terminator", "special"):
        subprocess.run([sys.executable, __file__, f"--{regression}"], check=True)

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
    if len(sys.argv) in (8, 9) and sys.argv[1] == "--worker":
        worker(
            sys.argv[2],
            sys.argv[3],
            sys.argv[4],
            sys.argv[5],
            int(sys.argv[6]),
            sys.argv[7],
            int(sys.argv[8]) if len(sys.argv) == 9 else 0,
        )
    else:
        main()
