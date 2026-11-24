# Geranium

Geranium runs persistent Linux containers from registry images or unpacked root
filesystems. It is written in C and runs without a daemon. Each container keeps
its own writable filesystem between starts.

**Use trusted workloads only. Geranium is not a production security sandbox.**
It requires Linux 5.3 or newer, user/mount/PID namespaces, and permission to use
ptrace.

## Build

On Alpine 3.23, install dependencies as root:

```sh
apk add build-base openssl-dev zlib-dev libarchive-dev zstd-dev yyjson-dev skalibs-dev ca-certificates
```

From the repository directory:

```sh
sh configure
make
```

On other distributions, install a C99 compiler, make, and development libraries
for OpenSSL, zlib, libarchive, Zstandard, yyjson, and skalibs. Runtime programs
do not need Python.

## Run a container

Run these commands from the build directory:

```sh
./gu-pull hello-world:latest
./gu-run --image hello-world:latest hello
```

The image prints `Hello from Docker!`. `gu-run` waits for the workload to exit.
The container remains available afterward. Restart it, read its saved output,
or remove it:

```sh
./gu-start hello
./gu-logs hello
./gu-rm hello
```

State defaults to `$HOME/.local/state/geranium`. Use `--root /absolute/path` on
each command to keep a separate store. Store state on a local filesystem.

## Commands

Each command is a separate executable. Run `./gu-run --help`, for example,
to see its options.

| Command | What it does |
| --- | --- |
| `gu-pull` | Download a public registry image. |
| `gu-run` | Create and start a container. |
| `gu-create` | Create a container without starting it. |
| `gu-start` | Start an existing container with its saved command. |
| `gu-stop` | Stop a running container. |
| `gu-logs` | Print output from the latest execution. |
| `gu-inspect` | Show the container's name and status. |
| `gu-rm` | Remove a stopped container and its writable filesystem. |
| `gu-rmi` | Remove a cached image without affecting existing containers. |

Use `-d` with `gu-run` or `gu-start` to detach after successful exec.
Workload commands must use absolute paths unless supplied by an image's defaults.

## Image compatibility

OCI and Docker v2 images support uncompressed, gzip, and Zstandard layers,
including PAX and GNU tar headers. Downloads stream into staged files rather than
buffering entire layers in each worker. Compressed digests, declared sizes, and
uncompressed DiffIDs are checked before publication. Path traversal and corrupt
archives still fail the pull.

For example:

```sh
./gu-pull alpine:edge
./gu-run --image alpine:edge alpine /bin/sh -c 'cat /etc/alpine-release'
./gu-pull golang:latest
./gu-run --image golang:latest go /usr/local/go/bin/go version
```

Geranium uses one UID/GID mapping: UID/GID 0 inside the container maps to the
invoking user outside it. Image ownership and non-root `User` settings are
adapted to that identity with a warning, not preserved. Set-ID bits are stripped;
unsupported extended attributes and archive device/FIFO/socket entries are
omitted with warnings. Programs that require multiple Unix identities can still
fail at runtime.

Each execution gets a private `/proc`, `/dev`, `/dev/pts`, and `/dev/shm`. The
small standard device set is bind-mounted individually; the host's `/dev` tree
is never exposed. `/tmp` is created when missing and remains persistent.

`ExposedPorts`, `Volumes`, `Healthcheck`, and custom `StopSignal` settings warn
when present. Ports are not published, volume paths remain ordinary container
directories, health checks are not run, and `gu-stop` sends SIGTERM.

## Current limits

- Registry access is anonymous. Private registry credentials are not supported.
- Images must target Linux and a supported host architecture. There is no
  architecture emulation or general multi-user runtime support.
- Geranium does not configure networking or manage cgroups, seccomp, or
  capabilities. Workloads share the host's network namespace.
- Pull limits are 2 GiB per compressed layer, 4 GiB of expanded tar data per image,
  and 1 GiB per file. Oversized images fail explicitly rather than bypass limits.
- Logs keep the first 1 MiB of output per execution. Extra output is discarded.

## Development

Run `make test` after building. The full Linux isolation suite needs a static
BusyBox fixture and permitted namespaces.

[CONTRIBUTING.md](CONTRIBUTING.md) covers code style and the locked uv/Ruff/ty
checks. [DESIGN.md](DESIGN.md) explains the architecture.

Geranium is released under the [Unlicense](LICENSE).
