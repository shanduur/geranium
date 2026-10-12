#!/bin/sh
set -eu
w=$(mktemp -d)
trap 'rm -rf "$w"' 0
cp -R src tests configure "$w/"
mkdir "$w/compiler space"
cat > "$w/header.h" <<'HEADER'
extern char *getenv(const char *);
extern int link(const char *,const char *);
static void __attribute__((constructor)) toolchain_executed(void) {
    const char *path=getenv("TOOLCHAIN_EXEC_LOG");
    if(path) { int rc=link(getenv("TOOLCHAIN_EXEC_SOURCE"),path); (void)rc; }
}
HEADER
cat > "$w/compiler space/cc" <<'WRAP'
#!/bin/sh
set -eu
printf 'compiler=%s args=%s\n' "$0" "$*" >> "$TOOLCHAIN_LOG"
case "$*" in *tests/*.c*)
 case "$*" in *-include*) ;; *) echo 'fixture lost configured CPPFLAGS' >&2; exit 98;; esac;;
esac
"${TOOLCHAIN_REAL_CC:-cc}" "$@"
prev=
for arg do
 if [ "$prev" = -o ]; then
  case "$arg" in .configure.*/*) chmod a-x "$arg";; esac
 fi
 prev=$arg
done
WRAP
chmod +x "$w/compiler space/cc"
printf 'executed\n' > "$w/exec-source"
export TOOLCHAIN_EXEC_SOURCE="$w/exec-source"
export TOOLCHAIN_EXEC_LOG="$w/executed.log"
export TOOLCHAIN_LOG="$w/compiler.log" TOOLCHAIN_REAL_CC="${CC:-cc}"
for maker in make bmake; do
 command -v "$maker" >/dev/null || { echo "$maker required" >&2; exit 1; }
 (cd "$w" && CC="$w/compiler space/cc" CPPFLAGS="-include $w/header.h" CFLAGS='-O0 -Wall -Wextra -Werror' LDFLAGS='-Wl,--as-needed' LDLIBS='-lm' sh configure)
 test ! -e "$w/executed.log"
 # Exercise the exact generated recipe without recursively running this test.
 python3 - "$w/Makefile" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read(); lines=s.splitlines()
recipe='\n'.join(x for x in lines if any('sh tests/'+name+'.sh' in x for name in ('readdir-eio','removal')))
open(p,'w').write(s+'\nfixture-test: all\n'+recipe+'\n')
PY
 (cd "$w" && env -u CC -u CPPFLAGS -u CFLAGS -u LDFLAGS -u LDLIBS "$maker" fixture-test)
 grep 'tests/readdir-eio.c' "$w/compiler.log" | grep -- '-include' >/dev/null
 test -s "$w/executed.log"
 rm "$w/executed.log"
 # Command-line overrides must reach fixtures too.
 cp "$w/compiler space/cc" "$w/compiler space/override"
 (cd "$w" && "$maker" fixture-test "CC=$w/compiler space/override" "CPPFLAGS=-include $w/header.h -DOVERRIDE=1" "CFLAGS=-O0 -Wall -Wextra -Werror -DCFLAGS_OVERRIDE=1" "LDFLAGS=-Wl,--no-as-needed" "LDLIBS=-lm -ldl")
 grep 'tests/readdir-eio.c' "$w/compiler.log" | grep -- '-DOVERRIDE=1' >/dev/null
 grep 'tests/readdir-eio.c' "$w/compiler.log" | grep 'compiler space/override' | grep -- '-DCFLAGS_OVERRIDE=1' | grep -- '-Wl,--no-as-needed' | grep -- '-lm -ldl' >/dev/null
 test -s "$w/executed.log"
 rm "$w/executed.log"
 (cd "$w" && "$maker" fixture-test "CFLAGS=")
 last=$(grep 'tests/readdir-eio.c' "$w/compiler.log" | tail -n 1)
 case "$last" in *-O0*|*-O2*|*-Werror*) echo 'empty CFLAGS override lost' >&2; exit 1;; esac
 rm "$w/executed.log"
 echo "$maker configured toolchain and overrides PASS"
done
