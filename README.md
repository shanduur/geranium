# Geranium: daemonless containers and images

Geranium is Unlicensed C software for persistent Linux containers from an unpacked
rootfs or a registry image. It delivers nine independent executables: gu-pull,
gu-rmi, gu-create, gu-start, gu-run, gu-stop,
gu-inspect, gu-logs, and gu-rm. Build dependencies are a C99
compiler, a shared-library linker, OpenSSL development libraries (libssl/libcrypto),
zlib, yyjson (tested with 0.12.0), and skalibs development headers/libraries
(tested with 2.14.4.0). On Alpine 3.23 install
`openssl-dev zlib-dev yyjson-dev skalibs-dev`. Configure probes these
external dependencies by compiling and linking; it does not download them.
Runtime TLS uses the system CA trust store; OpenSSL also supplies SHA256.

Commands report short status messages on stderr. Pulls show manifest/config
lookup and numbered layer download, verification, and extraction phases; cached
pulls report `already present`. There are no percentages, terminal controls, or
background logging processes. Startup status means the workload acknowledged
exec, not that it has completed successfully; foreground commands still return
the workload exit code. `inspect` and `logs` keep their existing stdout without
status chatter.

```sh
CFLAGS='-O2 -Wall -Wextra -Werror -pedantic' sh configure
make
./gu-create --root /absolute/private/state --rootfs /absolute/rootfs demo /bin/busybox echo hello
./gu-start --root /absolute/private/state demo
./gu-logs --root /absolute/private/state demo
./gu-inspect --root /absolute/private/state demo
./gu-rm --root /absolute/private/state demo
```

`run [--root PATH] [-d] --rootfs PATH NAME COMMAND [ARG ...]` combines create
and start. `start [--root PATH] [-d] NAME` restarts the same writable rootfs.
The option `--root` precedes other arguments on each executable; its default is
$HOME/.local/state/geranium. Commands must be absolute paths within the rootfs.
Foreground start/run stream stored stdout/stderr and return the workload status.
Detached start/run return after successful exec. `stop NAME` sends TERM to init,
waits two seconds, then forces KILL if necessary. `logs NAME` prints the latest
execution's merged output. `inspect NAME` prints name and status; `rm NAME`
removes stopped state. Programs never invoke an implicit shell.

`pull [--root PATH] REFERENCE` acquires anonymous public OCI or Docker schema2
images over verified HTTPS. `rmi [--root PATH] REFERENCE` removes only the image.
`create` and `run` accept `--image REFERENCE` in place of `--rootfs PATH`:

```sh
./gu-pull --root /absolute/private/state hello-world:latest
./gu-run --root /absolute/private/state --image hello-world:latest hello
./gu-rmi --root /absolute/private/state hello-world:latest
./gu-start --root /absolute/private/state hello
./gu-rm --root /absolute/private/state hello
```

References support `registry/repository:tag` and `repository@sha256:DIGEST`;
short names default to Docker Hub's `library/` and `latest`. Pull is idempotent
for an existing local reference; remove it first to refresh a mutable tag.
An image supplies Entrypoint+Cmd, Env, and WorkingDir. An explicit absolute command
replaces Entrypoint+Cmd and retains Env and WorkingDir. Image workloads receive
only image Env; rootfs workloads keep their existing `PATH=/bin:/usr/bin` environment.
Default commands may use a basename searched in image PATH. Non-root User,
non-Linux images, unsupported architectures, and ambiguous platform selections
fail explicitly. Linux amd64 and arm64/v8 are the supported image platforms.

Image layers support SHA256 descriptors/DiffIDs, gzip or plain ustar, regular
files, directories, symlinks, and hardlinks to existing regular files inside the
assembled rootfs. Links remain within the independent container copy. All image
entries must have UID/GID 0, mapped to the caller's UID/GID in storage and root
inside the single-ID namespace. Multi-user images are rejected rather than
flattened; current official BusyBox tags with non-root directory ownership are
therefore unsupported. Directory sticky bits are supported for images; the
stricter rootfs-copy interface above is unchanged. Set-ID bits, devices, PAX/GNU
extensions, zstd, and nondistributable layers are rejected. Whiteouts are applied
before additions in each layer, so opaque markers preserve that layer's additions.
Duplicate canonical member paths are rejected. Extraction and cleanup never
traverse a symlink parent. Non-root callers need owner-readable files and
owner-readable/searchable directories when copying an image; otherwise create
fails rather than widening the stored source permissions.

Limits: 128 layers; 64 MiB compressed bytes per layer; 1 GiB total uncompressed
tar bytes; 256 MiB per regular tar member; 100,000 entries per layer; depth 32;
16 MiB layer-path and copy-hardlink metadata limits;
1 MiB each for manifest/config/token JSON; 32 KiB HTTP headers; 32 JSON nesting
levels (root at depth zero); 32,768 JSON values, excluding object keys;
64 bytes per number token; five request attempts through redirects. JSON is
strictly decoded by external yyjson, with valid UTF-8, no embedded NUL in
strings/keys, and no duplicate decoded object keys at any depth. Unsigned image
integers require decimal digits only, with exact overflow checks. HTTP framing
requires Content-Length or chunked transfer; ambiguous framing, chunk extensions,
trailers, and truncated responses fail. Connection, TLS and headers share a
30-second monotonic deadline. Bodies allow five minutes of total elapsed time
and at most 30 seconds without I/O progress; progress never extends the total
body cap. Interrupted polling recomputes the remaining deadline. POSIX synchronous
DNS resolution cannot be cancelled portably: resolver delays may exceed the
connection window. Pull has no whole-operation deadline. TLS verifies hostname and CA;
redirects cannot downgrade HTTPS, and cross-origin redirects drop Authorization.
Only anonymous Bearer challenges are supported. `--http-loopback` explicitly enables
HTTP fixtures with literal `127.0.0.1`; it is never an automatic fallback.

Linux layer downloads and compressed digest checks use up to four owned fork
workers, automatically limited by the caller's CPU affinity (online CPU count
with a one-CPU fallback) and layer count. Completed layers keep their slots until
the parent extracts them in manifest order. Each worker reserves at most 64 MiB + 1 byte for a layer response, plus
1 MiB + 1 byte during auth refresh: at most 260 MiB + 8 bytes of worker response
buffers. Compressed staging files total at most 256 MiB; the parent maps one
file of at most 64 MiB. Parent manifest/config response buffers add 2 MiB + 2
bytes during fan-out. These are not total staging or RSS bounds: extraction
also stages an expanded tar under the existing 1 GiB expansion limit and
allocates separately, as do TLS, JSON, inherited metadata, and filesystem cache. There is no CLI setting. Workers refresh private auth state,
never extract or publish, and are cancelled and reaped before a failed or
interrupted pull releases its image lock. SIGKILL or a parent crash triggers
Linux's parent-death SIGKILL guard; orphan zombies then need the system reaper.
Backends without this worker lifetime guarantee retain the serial fetch path;
existing image platform validation still applies. Caller SIGCHLD actions stay
unchanged and notifications remain blocked until owned workers are reaped and the
caller mask is restored. SIG_IGN or SA_NOCLDWAIT callers use serial acquisition to
preserve auto-reaping. Worker fatal exits bypass inherited caller exit callbacks.
SIGINT/SIGTERM interrupt active workers unless ignored by the caller; ignored
actions remain effective in the parent and workers throughout acquisition.
Extraction finishes its current synchronous step before the parent handles
cancellation. An
interrupted unpublished stage is recovered by the next pull.

Images live under the private state's `.images/`, with retained `.lock-HASH`
fcntl anchors independent of container name locks. Pull stages `.pending-HASH`
and atomically renames a validated image into place. Removal unpublishes it to
that stage before cleanup. Interrupted stages are reclaimed under the same lock.
Create holds the image lock through the entire filesystem copy; container files
and runtime configuration then belong to that container, so rmi cannot damage it.
Metadata/directory sync errors fail. Atomic publication is supported; full
rootfs crash durability and protection from same-user store tampering are not.

Linux 5.3 or newer, enabled user/mount/PID namespaces, pidfds, and pivot_root
are required. Namespace
PID 1 owns lifecycle, reaping, and bounded log draining. The launcher exits;
there is no outside supervisor or background helper. A foreground client's
ownership pipe closes on client death; init terminates the workload and its
namespace descendants. Runtime identity comes from a held POSIX fcntl lock;
stop opens a pidfd and revalidates that same lock before signaling.
An init killed before completing result publication is reconciled to exited with
unknown status (no exitcode file); a foreground client returns 125. Completion
is accepted only after init releases runtime ownership, under the same per-name
lock used by inspect and stop. Log read or client output errors return 1, taking
precedence over the workload exit code; this includes final unread log data and
buffered inspect/help output. A write to a closed stdout pipe may instead
terminate the client via SIGPIPE when that signal is unblocked and has its default
disposition. The short-lived bootstrap launcher is guarded
by a kernel parent-death signal; it cannot outlive a cancelled caller.
A foreground attachment lock fences restart/removal until that client finishes
reading its generation; killing the client releases it. Normal exit writes exitcode; failed exec returns 127 (missing) or 126 (denied).

State must live on a local filesystem used by one host kernel, owned by the
invoking user, mode 0700. Names are 1–63 ASCII letters,
digits, underscores, or hyphens, and must not start with a hyphen. Directory paths
must be absolute without dot
components or symlink ancestors. Creation stages and atomically publishes the
copy; failed stages are removed on retry. Rootfs sources enclosing state are
rejected. Copy supports directories, regular files, and symlinks, preserving
permission bits. It rejects other file types, multiple owners/groups, hardlinked
regular files, and set-id/sticky modes. Every entry must match the caller's
UID/GID; the single-ID namespace maps these to root. Limits: depth 32,
100,000 entries, 1 GiB per regular file, 64 arguments, 8192 argument bytes.
There is no aggregate copy-size quota. Logs retain the first 1 MiB per execution;
excess output is drained and discarded. Work per logging tick is bounded.

Configure is POSIX sh and performs compile/link probes only. It honors CC,
CPPFLAGS, CFLAGS, LDFLAGS, LDLIBS, PREFIX, and --prefix=PATH. The generated
Makefile uses POSIX make syntax; `make gu-start` builds that program alone.
The core is compiled once as PIC objects into `libgeranium.so`; each executable
owns its `main` and calls one public function from `geranium.h`.
`make install DESTDIR=/temporary/path` installs the nine executables in
`PREFIX/bin`, the shared library in `PREFIX/lib`, and `geranium.h` in
`PREFIX/include`. The ELF SONAME is `libgeranium.so`; command loader paths are
`$ORIGIN:$ORIGIN/../lib`, so build-tree and relocated staged installs work without
`LD_LIBRARY_PATH` or ldconfig. Keep bin and lib together when moving an install.
The installed `geranium.h` exposes nine typed operations and
`geranium_open_state_root(root)`, which opens and validates the state directory.
The caller owns the returned descriptor. CLI syntax failures use this helper to
preserve state-root preparation and error precedence without a library parser.
For example,
`geranium_create(root, GERANIUM_SOURCE_ROOTFS, source, name, count, command)`
accepts a rootfs source path and workload arguments; `run` and `start` also
accept `detached`. `stop`, `inspect`, `logs`, and `rm` take root and name;
`rmi` takes root and image reference; `pull` takes root, image reference, and
`http_loopback`. Each CLI main owns its option parsing, help, and
HOME-based default root. Direct callers must supply an explicit root. Library
calls retain process lifetime and may exit on errors, fork, or change process
state; this is not a reentrant embedding API.
Configure discovers ELF or Mach-O through compile/link probes. Mach-O uses
`libgeranium.dylib`, `@rpath/libgeranium.dylib`, and `@loader_path` search paths;
ELF linkers use export maps, with a native illumos/Solaris linker alternative.
These non-Linux build paths require target qualification; they do not establish
working native backends. Static linking of the core/commands is rejected;
static workload fixtures remain required.
GERANIUM_BACKEND=unsupported builds portable storage/CLI support and rejects
execution before launching lifecycle processes. This does not provide native
execution on BSD, Darwin, or illumos, or Linux-image compatibility there.

`make test` runs 37 selected shell gates, including all 23 registered common
image cases and the separate image-copy/removal race gate. A selector-completeness
check rejects unregistered image cases. The library gate verifies the actual
ELF linkage/API, an installed C consumer, moved installation loading, libc fault
interposition, and missing-library failure. A static BusyBox fixture and permitted
Linux namespaces are required; isolation failure is a failed gate. The authority
case always checks production HTTP/TLS request headers and redirects on remapped
unprivileged ports. The actual default-port TLS wire case may report a permission
skip for a non-root caller; root callers and `GERANIUM_REQUIRE_DEFAULT_HOST=1`
require it to pass. The lower-directory permission regression requires a non-root
caller and runs in the runner gate. For the shared musl core, static workload fixtures, BSD make, root and
non-root gates:

```sh
docker run --rm --privileged -v "$PWD:/src:ro" alpine:3.23 sh /src/tests/musl.sh
```

The script installs build dependencies only in the disposable container.
Native GCC static fixture compilation also needs static libc available.

Remaining scope: private registry credentials, richer image formats and ownership
mappings, networking configuration, /proc and device setup, native backends for
other kernels, aggregate container-copy quotas, crash-durable syncing of rootfs
contents, and richer inspection. No seccomp, capability policy, or cgroups are
implemented. This candidate
is not a production security sandbox. It trusts same-user state access and
rootfs content; rootless namespaces do not establish a hostile-workload security
contract. Foreground output can block its client, but does not block init.

Bootstrap and init fatal diagnostics are retained in the container log, with a
separate maximum of 512 bytes per startup in addition to the 1 MiB workload
output budget. Detached startup leaves the caller session and process group;
foreground cancellation remains tied to the client ownership pipe. Root switching
uses no reserved directory inside the persistent rootfs.

Linux startup requires permission for the payload to use `PTRACE_TRACEME`
and for its namespace PID 1 parent to trace it until `PTRACE_EVENT_EXEC`.
PID 1 detaches immediately after that kernel event; no external tracer or
supervisor remains. Restrictive ptrace/Yama or syscall-filter policies can
reject startup, which fails with a diagnostic rather than accepting pipe EOF
as proof of exec. External debuggers that follow forks can conflict with this
short tracing interval because a process can have only one tracer. Detached
success proves exec occurred, even when the executable exits immediately; it
does not promise the workload is still running when the client returns.
During exec readiness, PID 1 observes foreground ownership and loss of the
startup acknowledgement reader, and enforces an eight-second monotonic deadline
(the frontend waits twelve seconds). Cancellation or timeout kills and reaps the
owned payload, publishes failure, and releases the runtime lock before replying
to a remaining reader. A task stuck in uninterruptible kernel sleep can delay
reaping despite SIGKILL; this is not a guarantee of bounded kernel teardown.

The full namespace/ptrace fault suite is Linux-only. It needs Python 3 and the
OpenSSL command for controlled registry,
TLS, and race fixtures. `sh tests/public-image.sh` separately verifies Docker Hub
HTTPS acquisition and a complete image-backed lifecycle. The musl script runs this
public gate as root and runner, in addition to the same 37 selected shell gates.
Its root gate requires the real port-443 wire test; its runner gate exercises
rootless lower-directory updates.
Runtime programs have no Python dependency. Command lock anchors are retained as
`.command-NAME.lock` in the private state root even after removal or a failed
lookup; see DESIGN.md for the retention and serialization contract.

Removal unpublishes `NAME` atomically to `.pending-NAME` while retaining its
stable command lock, before deleting any contents. An interrupted removal leaves
that private stage for the next `create`/`run` or `rm` of the same name to reclaim.
If only the stage exists, `rm` reclaims it (including an abandoned creation);
if neither exists, `rm` fails. Published and staged runtime/foreground ownership
must be absent before cleanup. Unsafe or occupied stages fail without removing
the published instance. Command lock anchors remain across removal/recreation.

Atomic name removal is distinct from crash durability: directory fsync failures
are reported, including after rename or final stage removal. A nonzero `rm` can
therefore leave the name absent; retry checks the actual remaining stage. Success
requires complete cleanup and successful directory synchronization.
