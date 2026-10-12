#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
export PORTABLE_REAL_CC="${CC:-cc}"
cp -R src configure tests "$work/"
# Static command/core linking is explicitly rejected before replacing Makefile.
for variable in CPPFLAGS CFLAGS LDFLAGS LDLIBS; do
    if (cd "$work" && env "$variable=-static" sh configure > "$work/static-error" 2>&1); then
        echo "configure accepted $variable=-static" >&2; exit 1
    fi
    grep 'configure: -static is unsupported' "$work/static-error"
    test ! -e "$work/Makefile"
done
# Generated target probes are made non-executable; configure must never run them.
cat > "$work/compiler" <<'WRAP'
#!/bin/sh
set -eu
"$PORTABLE_REAL_CC" "$@"
prev=
for arg do
 if [ "$prev" = -o ]; then chmod a-x "$arg"; fi
 prev=$arg
done
WRAP
chmod +x "$work/compiler"
(cd "$work" && CC="$work/compiler" CPPFLAGS='-DGERANIUM_CONFIGURE_SENTINEL=1' CFLAGS='-O0 -DGERANIUM_CFLAGS_SENTINEL=1' LDFLAGS='-Wl,--as-needed' LDLIBS='-lm' PREFIX=/opt/example sh configure && grep 'CPPFLAGS = -DGERANIUM_CONFIGURE_SENTINEL=1' Makefile && grep 'CFLAGS = -O0 -DGERANIUM_CFLAGS_SENTINEL=1' Makefile && grep 'LDFLAGS = -Wl,--as-needed' Makefile && grep 'LDLIBS = -lm' Makefile)
(cd "$work" && CC="$PORTABLE_REAL_CC" CPPFLAGS='-DGERANIUM_CONFIGURE_SENTINEL=1' CFLAGS='-O0 -Wall -Wextra -Werror' LDFLAGS='-Wl,--as-needed' LDLIBS='-lm' sh configure --prefix=/opt/example && make && make install DESTDIR="$work/install")
for v in create start run stop inspect logs rm pull rmi; do [ -x "$work/install/opt/example/bin/gu-$v" ]; [ ! -L "$work/install/opt/example/bin/gu-$v" ]; done
[ ! -e "$work/install/opt/example/bin/geranium" ]
mkdir "$work/install-fixture"
"$work/install/opt/example/bin/gu-create" --root "$work/install-state" --rootfs "$work/install-fixture" installed /bin/true
"$work/install/opt/example/bin/gu-inspect" --root "$work/install-state" installed | grep status=created
"$work/install/opt/example/bin/gu-rm" --root "$work/install-state" installed
(cd "$work" && GERANIUM_BACKEND=unsupported CFLAGS='-O2 -Wall -Wextra -Werror' sh configure && make && mkdir fixture && ./gu-create --root "$work/state" --rootfs "$work/fixture" x /bin/true && ./gu-inspect --root "$work/state" x && if ./gu-start --root "$work/state" x > "$work/error" 2>&1; then echo 'unsupported backend executed' >&2; exit 1; fi && grep 'isolation unsupported' "$work/error" && ./gu-inspect --root "$work/state" x | grep status=created && ./gu-rm --root "$work/state" x)
(cd "$work" && sh tests/lifecycle.sh && sh tests/safety.sh && sh tests/invalid.sh)
# Reproduce coarse filesystem/make timestamps without timing-sensitive sleeps.
# Switching backend must work even when Makefile and old products have equal mtimes.
for maker in make bmake; do
    command -v "$maker" >/dev/null 2>&1 || continue
    build="$work/reconfigure-$maker"
    mkdir "$build"
    cp -R src configure "$build/"
    (cd "$build" && CC="$PORTABLE_REAL_CC" CFLAGS='-O0' sh configure && "$maker" -j4)
    python3 - "$build" <<'PY'
import pathlib,sys,os
p=pathlib.Path(sys.argv[1]); files=[*p.glob('*.o'),*p.glob('libgeranium.*'),*p.glob('gu-*')]
t=max(x.stat().st_mtime_ns for x in files)
(p/'clock').write_text(str(t))
for x in files: os.utime(x,ns=(t,t))
PY
    (cd "$build" && CC="$PORTABLE_REAL_CC" GERANIUM_BACKEND=unsupported CFLAGS='-O0' sh configure)
    python3 - "$build" <<'PY'
import pathlib,sys,os
p=pathlib.Path(sys.argv[1]); t=int((p/'clock').read_text()); os.utime(p/'Makefile',ns=(t,t))
PY
    (cd "$build" && "$maker" -j4 && mkdir fixture && ./gu-create --root "$build/state" --rootfs "$build/fixture" x /bin/true)
    if "$build/gu-start" --root "$build/state" x > "$build/error" 2>&1; then
        echo "$maker reused Linux backend after reconfigure" >&2; exit 1
    fi
    grep 'isolation unsupported' "$build/error"
    "$build/gu-inspect" --root "$build/state" x | grep status=created
    "$build/gu-rm" --root "$build/state" x
    echo "$maker equal-timestamp backend reconfiguration PASS"
done
echo 'portable configure, install and unsupported backend PASS'
