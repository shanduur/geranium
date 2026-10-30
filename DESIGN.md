# Geranium design

Geranium manages persistent containers without a daemon. It targets trusted
workloads on Linux, not hostile tenant code. See the [README](README.md) for
build instructions and usage.

## Programs and shared core

Nine separate `gu-*` programs provide the command-line interface, following
[XBPS](https://github.com/void-linux/xbps). Each program owns its options, help,
and defaults, then calls a typed operation in `geranium.h`.

One shared C library implements storage, images, and container lifecycle. The
core validates inputs independently and does not parse CLI arguments. Its API
retains command-process lifetime assumptions; it is not a reentrant embedding API.
Relative loader paths allow installed programs and the library to move together.

## Runtime ownership

The Linux backend uses user, mount, and PID namespaces. The container's PID 1
reaps descendants, captures output, and owns workload termination. No supervisor
remains outside the container after detached startup.

- Startup succeeds only after the kernel confirms workload execution.
- Foreground execution returns the workload status. Losing the foreground client
  terminates its workload and descendants.
- Detached execution returns after startup. It does not guarantee the workload
  is still running when the command returns.
- Stop verifies runtime ownership and process identity before signaling. A saved
  PID alone is never proof that a container is alive.

Logs retain bounded output from the latest execution. Separate runtime and
foreground-attachment locks prevent removal or restart while an execution is
still owned.

## Persistent storage

Each container owns its configuration and writable root filesystem. Restarting
reuses that filesystem. Creating from an image copies its contents, so removing
the cached image cannot damage existing containers.

Commands serialize changes with per-container filesystem locks. Image operations
use per-reference locks. Lock files remain after removal because replacing a lock
file could allow concurrent owners.

Creation, pull, and removal use private staging directories and atomic renames.
Incomplete stages are not usable containers or images. Later operations recover
interrupted stages. Cleanup must not follow image-controlled symlinks onto the
host or remove files still owned by a running instance. Atomic publication does
not promise full rootfs crash durability.

## Image verification

Registry requests verify HTTPS and constrain redirects and authentication tokens.
Pull verifies descriptor sizes, content digests, and uncompressed layer DiffIDs
before publishing an image. Extraction applies whiteouts and rejects filesystem
traversal and unsupported ownership or metadata.

Linux downloads layers with bounded, short-lived workers. The parent applies
layers in manifest order and owns publication. Failure cancels and reaps its
workers before releasing the image lock.

Saved image configuration supplies command defaults, environment, and working
directory. Unsupported platforms or configuration fail before execution.
Accepting OCI images does not imply full OCI runtime conformance.

## Portability and boundaries

Storage and lifecycle policy use POSIX interfaces. Kernel isolation and process
identity belong to platform backends. Only Linux currently runs containers;
unsupported backends fail rather than execute a workload without isolation.
A portable image format does not make Linux executables run on other kernels.

The build targets musl first and uses handwritten POSIX shell and make files.
OpenSSL, zlib, yyjson, and skalibs are external dependencies. Geranium does not
implement TLS cryptography itself.

Geranium does not manage networking, devices, cgroups, seccomp, or capability
policy. Rootless namespaces are not a hostile-workload security guarantee.
