#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' 0 1 2 3 15
cp configure "$work/"
export DEPENDENCY_REAL_CC="${CC:-cc}"
cat > "$work/compiler" <<'WRAP'
#!/bin/sh
set -eu
case "$DEPENDENCY_NAME" in
    yyjson) library=-lyyjson; header=yyjson.h;;
    skalibs) library=-lskarnet; header=skalibs/strerr.h;;
esac
for arg do
    if [ "$DEPENDENCY_MISSING" = library ] && [ "$arg" = "$library" ]; then
        echo "fixture: $DEPENDENCY_NAME library unavailable" >&2
        exit 1
    fi
    case "$arg" in
        *.c)
            if [ "$DEPENDENCY_MISSING" = header ] && grep -F "#include <$header>" "$arg" >/dev/null; then
                echo "fixture: $DEPENDENCY_NAME header unavailable" >&2
                exit 1
            fi;;
    esac
done
"$DEPENDENCY_REAL_CC" "$@"
WRAP
chmod +x "$work/compiler"
for dependency in yyjson skalibs; do
    for missing in header library; do
        status=0
        (cd "$work" && CC="$work/compiler" DEPENDENCY_NAME="$dependency" DEPENDENCY_MISSING="$missing" sh configure) > "$work/error" 2>&1 || status=$?
        if [ "$status" -ne 1 ]; then
            cat "$work/error"
            echo "configure accepted missing $dependency $missing" >&2
            exit 1
        fi
        grep "fixture: $dependency $missing unavailable" "$work/error"
        grep "configure: $dependency development library required" "$work/error"
        test ! -e "$work/Makefile"
    done
done
echo 'missing yyjson/skalibs headers and libraries rejected independently PASS'
