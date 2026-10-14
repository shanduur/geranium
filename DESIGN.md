# Geranium design

The README and executable tests describe the implemented lifecycle and image
increment. Future native backends and broader OCI compatibility remain design scope.

## Scope

Geranium manages persistent containers and images through nine independent programs: `gu-pull`, `gu-run`, `gu-start`, `gu-stop`, `gu-rm`, `gu-rmi`, `gu-create`, `gu-logs`, and `gu-inspect`.
The interface follows [XBPS](https://github.com/void-linux/xbps): separate executable entry points with shared C internals, consistent options, and filesystem-level locks, not subcommands or a multicall dispatcher.
All storage, lifecycle, image, registry, JSON, extraction, and backend code resides
in one dynamically shared library, compiled once as PIC objects. The nine tiny
main translation units own option definitions, parsing, help, and CLI defaults;
they call corresponding typed geranium_* operations from geranium.h. The core
has no verb-string dispatcher and validates direct callers independently. The
command vector is workload data, not CLI syntax. Error paths retain process
lifetime, output, and resource ownership; this is not a reentrant embedding API.
Export maps limit the library ABI to those nine operations plus
`geranium_open_state_root`.
ELF uses libgeranium.so with that SONAME and relative $ORIGIN loader paths;
Mach-O uses libgeranium.dylib with an @rpath install name and @loader_path paths.
Configure discovers format and linker conventions without executing target code.
The implementation is C, targets musl on Linux first, and uses handwritten POSIX shell configuration and a handwritten Makefile.
There is no seccomp, capability configuration, cgroup management, daemon or external background supervisor, or overlay filesystem requirement.
Lifecycle init, output handling, and reaping must remain inside the container's PID namespace.
The detached launcher must exit before startup returns; only container processes remain.
The intended workloads are trusted applications, not hostile tenant code.

## Portable core and native backends

Storage, command parsing, configuration validation, and lifecycle policy should use POSIX interfaces.
Kernel isolation and process identity belong to an explicit platform backend.
The Linux backend uses user, mount, and PID namespaces and root switching.
Non-POSIX extensions are permitted only where the operating-system backend or a documented filesystem feature requires them.
Unsupported backends must return an actionable error before starting a workload, never run it unisolated.

Container-image format portability is distinct from executable compatibility.
A Linux executable still needs a compatible Linux environment, irrespective of the host compiler or the image's OCI packaging.
BSD, Darwin, and illumos are consideration targets, not supported runtime claims in the first increment.
BSD variants require separate investigation rather than a single assumed jail implementation.
Native kernel backends would require compatible native userspace images; Linux images on other kernels need a supported compatibility environment or Linux VM.
Musl is the Linux target libc, not a cross-kernel abstraction.

## Lifetime and authority

Published container configuration and its writable root filesystem own the persistent container definition.
A container instance can be created without being started and restarted without discarding filesystem changes.
A running instance has one supervising owner, which captures output and observes termination.
Persisted process numbers alone are not proof of a live container; process identity and execution generation must be verified before signaling.
Inspection must distinguish a failed or stale running record from verified liveness.

Commands serialize mutations of the same container while permitting inspection, logs, and stop during execution.
Command serialization uses a persistent hidden `.command-NAME.lock` regular file
in the validated private state root, with POSIX `fcntl` write locks. Each name's
lock covers existence checks, stale-stage cleanup, copy, atomic publication,
startup acknowledgement, stop waits and refresh, and removal. Different names
have independent pathnames, so these operations need no root command lock.
Commands never unlink these anchors, including on failure or removal: replacing
a lock inode could let an old waiter and a new opener act on the same name
concurrently. Anchors are retained even for valid names that do not exist.
The 63-byte name bound does not impose a practical finite bound on this storage;
the number of retained files grows with distinct names used. Reclamation requires
an offline, quiescent state-root disposal; there is no online anchor collector.
Logs release the name lock after opening the log and before streaming. Foreground
startup releases it after acknowledgement while retaining its separate attachment
generation fence. Runtime ownership and backend process identity remain separate
from command serialization. Stop retains the name lock through its bounded wait.

Removal must not race a restart or silently discard a running instance.
Stopping must terminate descendants, not merely the original command's process group.
On Linux a private PID namespace provides that lifetime boundary without cgroups.
Logs need a defined storage bound and explicit behavior at that bound.

Creation and image acquisition publish only complete, validated results.
Interrupted operations may leave owned staging data, but it must not be resolved as a usable container or image.
Cleanup must neither follow image-controlled symlinks onto the host nor delete data still owned by another instance.
Filesystem-copy fidelity and supported UID/GID mappings must be documented rather than silently treating all images as single-user.

## Image boundary

The OCI image specification defines descriptors, manifests, configuration, and layers.
The distribution specification defines registry operations; image-format support alone is not registry-client support.
Registry requests must verify TLS, constrain credentials and redirects, and verify downloaded content digests.
Layer assembly must also verify uncompressed DiffIDs, apply whiteouts correctly, and prevent filesystem traversal.
Image configuration supplies default command, environment, working directory, and user.
Unsupported platform or configuration requirements fail before execution.
Full OCI runtime conformance is not implied by accepting OCI images or borrowing runtime-spec terminology.

The implemented image store uses `.images/HASH` keyed by a normalized reference,
with persistent `.lock-HASH` POSIX fcntl anchors and `.pending-HASH` stages.
There is no root-wide network lock. Pull serializes only its reference, assembles
privately, checks every descriptor digest and size plus uncompressed DiffIDs,
and publishes by rename. Rmi uses the same lock, unpublishes before recursive
no-follow cleanup, and preserves anchors. Create retains the image lock while
copying, then persists an independent rootfs, command, and image configuration.
Removal does not require container-reference accounting because copies share no
inodes with images. Within each copy, validated image hardlinks remain hardlinks;
the original rootfs interface still rejects hardlinked sources and sticky modes.

All layer descriptors and their DiffID correspondence are validated before
fan-out. Linux uses a maximum of four affinity-sized fork workers and per-worker
EOF pipes polled together, so a later failure cancels earlier blocked requests.
The parent alone owns metadata, manifest-order extraction and publication;
workers only download, verify and fill preopened private staging files. Slot
reservations last through extraction, bounding both active and completed work.
Worker tokens and buffered strerr state are private after fork. The first child
operation registers a last-in, first-out atexit guard that calls _exit(1); failure
to register it calls _exit(1) directly. Startup and helper fatal exits therefore
cannot run inherited caller atexit/on_exit callbacks or delay completion EOF. Worker descriptors
exclude the image lock, rootfs and unrelated caller descriptors. The backend
registers Linux parent-death SIGKILL before worker I/O, checking the parent PID
again to close the registration race. Other backends use serial pulls rather
than leaving orphanable helpers. See README for buffer limits and crash limits.

The process-lifetime coordinator temporarily installs SIGINT/SIGTERM handlers
unless the caller ignores those signals; workers also retain ignored actions.
Caller-blocked SIGINT/SIGTERM remain blocked in parent and workers, with queued
signals preserved until the caller chooses to unblock them.
SIGCHLD keeps its inherited disposition and stays blocked until every owned worker
is reaped, then the caller's mask is restored. Pending notifications and unrelated
child exits during acquisition reach the caller's handler after restoration.
Callers using SIG_IGN or SA_NOCLDWAIT for SIGCHLD acquire layers serially, preserving
auto-reaping without risking recycled worker PIDs. Other caller actions and masks
are restored when done. Cancellation signals kill owned workers immediately; parent cleanup reaps each recorded PID
before releasing the lock. An atexit cleanup also covers fatal exits from existing
extraction and allocation/filesystem helpers. Reaping blocks cancellation signals
to prevent PID-reuse races. Caller-owned children are never waited on. SIGKILL
cannot run parent cleanup: the kernel terminates workers, system init must reap
orphan zombies, and the next lock holder recovers the unpublished stage. Current
synchronous extraction may delay parent signal exit until its step returns.

Caller-side status messages use external skalibs `strerr` (`-lskarnet`) for
plain newline-terminated stderr output. The API flushes each message and
preserves errno; it is synchronous and not thread-safe, matching the existing
process-lifetime entry points. Startup events follow the existing exec ACK,
without new delays or state reads. Pull completion follows atomic publication
or validated cache reuse. Messages contain fixed text and bounded layer counts,
not credentials, tokens, references, or server response bodies. The bounded PID1
fatal path and raw-byte container output capture remain unchanged. No public
API, callback, or logging configuration is added. Linux pulls own bounded
short-lived fetch workers; none survives the pull as an executing helper.

Registry transport is original POSIX socket code using system OpenSSL for verified
TLS and SHA256, zlib for gzip, and external yyjson for JSON decoding. No TLS
cryptography, curl, archive library, or third-party implementation is vendored.
Anonymous Bearer challenge requests carry no credentials. Cross-origin redirects strip tokens;
HTTPS downgrade redirects fail. Resource bounds and the synchronous resolver's
unbounded delay are explicit in README. No external runtime helper is introduced.

Private image/registry code borrows yyjson values directly from an owning
`yyjson_doc`; no separate JSON tree is built. The document stays alive until all
borrowed values/strings have been consumed or copied. The installed geranium.h
and exported ABI do not expose yyjson. A thin adapter rejects inputs over 1 MiB
before decoding and checks the decoded tree for depth (root zero, maximum 32),
32,768 values excluding keys, embedded NUL, and duplicate decoded keys at every
depth. yyjson's strict defaults handle grammar, full-document consumption, and
UTF-8/escape validation. `YYJSON_READ_NUMBER_AS_RAW` preserves numeric lexemes;
the adapter retains the 64-byte number limit and accepts unsigned integers only
as decimal digits within unsigned long long range. No permissive flags are used.
Decode occurs before traversal budgets are checked, under the input-size bound.
Duplicate checking uses yyjson's first-match object lookup, retaining the existing
quadratic worst case for wide objects. The core links `-lyyjson`; distribution
musl builds use the shared system library, without copying yyjson source.

Two tar passes per verified layer first apply whiteouts to lower contents, then
apply additions. This makes opaque semantics independent of tar entry order.
All parent traversal uses directory descriptors with O_NOFOLLOW, including
hardlink target lookup. Temporary directory permissions permit layer modification
and are restored, including opaque directories. Only single-root image ownership
is mapped; additional users, unsupported tar metadata, and unsupported image
configuration fail. Configuration is copied into container state and interpreted
again before startup. Payload chdir and PATH lookup happen after root switching;
execve receives only the saved image environment. This is a bounded OCI subset,
not broad OCI conformance or a hostile-workload isolation guarantee.

Prefer libc, then Skarnet libraries for justified functionality.
Minimal external TLS, compression, and JSON dependencies are permitted.
Do not write TLS cryptography from scratch or copy third-party source under an incompatible license.

## Alternatives

A runtime-bundle-only tool is smaller, but does not satisfy the requested image and container lifecycle.
Strict POSIX-only kernel calls cannot provide Linux namespace isolation.
The container's own init owns the running instance; detached execution must not retain an external background supervisor.
A Linux VM backend can preserve Linux image compatibility on other hosts, but adds provisioning and VM-lifetime responsibilities that need a separate decision.

## References

- [Lilipod](https://github.com/89luca89/lilipod): minimal feature scope and namespace-based isolation.
- [crun](https://github.com/containers/crun): C runtime design reference; source reuse requires its licensing obligations.
- [libocispec](https://github.com/containers/libocispec): typed OCI parsing reference, not a selected dependency.
- [OCI image specification](https://github.com/opencontainers/image-spec).
- [OCI runtime specification](https://github.com/opencontainers/runtime-spec).
- [OCI distribution specification](https://github.com/opencontainers/distribution-spec).
- [musl](https://musl.libc.org/about.html): libc implemented on Linux system calls.
- [Skarnet libraries](https://skarnet.org/software/skalibs/).
