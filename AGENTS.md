# Geranium engineering constraints

Geranium is a minimal container and image manager in C, licensed under the Unlicense.
The target programs are gu-pull, gu-run, gu-start,
gu-stop, gu-rm, gu-rmi, gu-create, gu-logs,
and gu-inspect. Follow XBPS's separate-program interface, not subcommands
or a multicall dispatcher. One real shared library contains the complete core;
compile its sources once as PIC objects. Each program owns its argument definitions, parsing, help, CLI defaults, and
main, and calls its corresponding typed geranium_* entry point from geranium.h.
Only help/name-token/standard-descriptor plumbing may be shared between CLI mains.
The root-opening API also takes only a root path; CLI failure paths use it to
preserve state preparation before syntax diagnostics. The core accepts operation
parameters, not argc/argv or option strings; it keeps
validation for direct callers. Entry points retain CLI process lifetime, not a
reentrant embedding API.
Install the library and public header with the programs; loader paths must allow
relocated PREFIX/DESTDIR installs without absolute build/staging paths. Use filesystem-level locks. No background helpers may remain outside
containers, including per-container supervisors. Lifecycle init/reaping/log handling
may run inside the container's PID namespace; the launcher must exit after detached
startup. Foreground client processes are not background services.
Deliver working vertical increments with explicit remaining scope, never successful placeholders.

## C formatting

Use clang-format 18 with the checked-in `.clang-format`, copied unchanged from
Linux v6.18. Apply it to production C sources, headers, and C test fixtures.
The kernel rules use eight-column tabs, an 80-column target, and function braces
on a separate line. Run `make fmt` and `make check-format`; override
`CLANG_FORMAT=clang-format-18` when the tool has a versioned name. Keep formatting
changes behavior-preserving. The upstream configuration retains its GPL-2.0
SPDX notice; Geranium's implementation remains under the Unlicense.

## Portability

- Target musl on Linux first, but preserve portability to BSD, Darwin, and illumos.
- Keep storage, command parsing, and lifecycle policy in POSIX C.
- Isolate Linux namespaces, mounts, root switching, and Linux process identity behind a backend interface.
- An unsupported platform must fail explicitly before running a workload. Never degrade to unisolated execution.
- Native platform support and execution of Linux images are different contracts. Other kernels need native images, an explicitly supported compatibility environment, or a Linux VM.
- Configure is handwritten POSIX sh; the handwritten Makefile uses POSIX make syntax. Do not require GNU make, Autotools, CMake, or Meson.
- Configuration uses compile/link probes, not execution of target binaries.

## Scope and dependencies

- No seccomp, capability management, or cgroup management.
- Prefer libc, then Skarnet libraries for justified extra functionality.
- Minimal external TLS and compression dependencies are explicitly approved where necessary.
- Use the named upstream projects as design references. Do not copy third-party code into the Unlicensed project without verifying and preserving applicable licensing obligations.
- Do not implement TLS cryptography from scratch.

## Verification

- Use vertical RED/GREEN tests against observable real-binary behavior.
- Run a musl build and actual namespace-isolation tests, separately from portable storage tests.
- Do not silently skip isolation tests or represent compiler portability as working platform support.
- Preserve rootfs ownership, process identity, locking, atomic publication, bounded logs, and safe cleanup as explicit contracts.
- Do not commit, push, or change remote settings unless requested.
