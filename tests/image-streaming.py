#!/usr/bin/env python3
"""Layer-sized streaming limits must not truncate HTTP control responses."""

import json
import tempfile

from images import Registry, call, encode, published_roots


def main():
    for control in ("redirect", "authenticate"):
        registry = Registry()
        layer_digest = json.loads(registry.manifest)["layers"][0]["digest"]
        blob_path = "/v2/fixture/blobs/" + layer_digest
        metadata = encode({"errors": [{"code": "UNAUTHORIZED", "message": "x" * 4096}]})
        assert len(metadata) > len(registry.blobs[layer_digest])
        if control == "redirect":
            registry.responses[blob_path] = (
                307,
                {"Location": "/redirected/" + layer_digest},
                metadata,
            )
        else:
            original_route = registry.route

            def route(
                path,
                headers,
                *,
                blob_path=blob_path,
                registry=registry,
                metadata=metadata,
                original_route=original_route,
            ):
                if (
                    path == blob_path
                    and headers.get("Authorization") != "Bearer fixture-token"
                ):
                    return (
                        401,
                        {
                            "WWW-Authenticate": (
                                f'Bearer realm="http://127.0.0.1:{registry.server.server_port}/token",'
                                'service="fixture",scope="repository:fixture:pull"'
                            )
                        },
                        metadata,
                    )
                return original_route(path, headers)

            registry.route = route
        with tempfile.TemporaryDirectory() as state, registry:
            call("pull", "--root", state, "--http-loopback", registry.ref)
            assert (
                published_roots(state)[0] / "etc/marker"
            ).read_bytes() == b"from registry\n"
            if control == "redirect":
                assert "/redirected/" + layer_digest in registry.requests
            else:
                assert any(path.startswith("/token?") for path in registry.requests)
        print(
            control,
            "metadata larger than compressed layer is handled independently PASS",
        )


if __name__ == "__main__":
    main()
