#!/bin/sh
# Build/run the published HMJE contracts without a Python harness.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HMJEC=${HMJEC:-$ROOT/bin/hmjec}
HMJE_PLATFORM_LIBRARIES=${HMJE_PLATFORM_LIBRARIES:-$ROOT/libraries}
HMJE_UI_ROOT=$ROOT/libraries/hmje-ui
export HMJE_PLATFORM_LIBRARIES HMJE_UI_ROOT
WORK=$(mktemp -d "${TMPDIR:-/tmp}/hmje-public-tests.XXXXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
for source in "$ROOT/tests/smoke.hmje" "$ROOT"/examples/*.hmje; do
    name=$(basename -- "$source" .hmje)
    if [ "$name" = screen ]; then
        "$HMJEC" --target posix-host-exe "$source" -o "$WORK/$name"
        echo "PASS $name (compile only)"
    else
        "$HMJEC" --target posix-host-exe "$source" -o "$WORK/$name"
        "$WORK/$name" > "$WORK/$name.out"
        cmp "$WORK/$name.out" "${source%.hmje}.expect"
        echo "PASS $name"
    fi
done
for library in hmje-host hmje-syntax; do
    case "$library" in hmje-host) artifact=HostSmoke ;; hmje-syntax) artifact=SyntaxSmoke ;; esac
    "$HMJEC" --project "$ROOT/libraries/$library" --artifact "$artifact" --target posix-host-exe -o "$WORK/$artifact"
    (cd "$WORK" && "./$artifact")
    echo "PASS $artifact"
done
if [ -f "$ROOT/libraries/hmje-syntax/Tests/EnumSyntaxSmoke/main.hmje" ]; then
    "$HMJEC" --project "$ROOT/libraries/hmje-syntax" --artifact EnumSyntaxSmoke --target posix-host-exe -o "$WORK/EnumSyntaxSmoke"
    (cd "$WORK" && ./EnumSyntaxSmoke)
    echo 'PASS EnumSyntaxSmoke'
fi
HMJEC="$HMJEC" sh "$ROOT/libraries/hmje-ui/Tests/ShapeWinding/run.sh"
