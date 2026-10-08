#!/bin/sh
# Packaging/host boundary: bind source provenance to the selected native bytes.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || {
    echo 'usage: sh tools/write-tool-revisions.sh BIN_DIRECTORY [VERIFIED_SOURCE_REVISION]' >&2; exit 2;
}
DIRECTORY=$(CDPATH= cd -- "$1" && pwd)
if [ "$#" -eq 2 ]; then
    REVISION=$2
elif git -C "$ROOT" rev-parse --verify HEAD >/dev/null 2>&1; then
    REVISION=$(git -C "$ROOT" rev-parse HEAD)
    # Installing generated seeds does not change the sources compiled into them.
    if ! git -C "$ROOT" diff --quiet HEAD -- compiler library libraries platforms bin tools platform-libraries.lock platform-libraries.internal.lock ||
       [ -n "$(git -C "$ROOT" ls-files --others --exclude-standard -- compiler library libraries platforms bin tools)" ]; then
        REVISION=$REVISION-dirty
    fi
elif [ -f "$ROOT/SOURCE-REVISION" ]; then
    REVISION=$(head -n 1 "$ROOT/SOURCE-REVISION")
else
    REVISION=unknown
fi
TEMP=$(mktemp "$DIRECTORY/.tool-revisions.XXXXXXXX")
trap 'rm -f "$TEMP"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
printf '# source-revision\tsha256\texecutable\n' > "$TEMP"
for NAME in 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기 frontend.exe backend.exe product-coordinator.exe project-coordinator.exe; do
    [ -f "$DIRECTORY/$NAME" ] || continue
    if command -v shasum >/dev/null 2>&1; then
        DIGEST=$(shasum -a 256 < "$DIRECTORY/$NAME")
    else
        DIGEST=$(sha256sum < "$DIRECTORY/$NAME")
    fi
    DIGEST=${DIGEST%% *}
    printf '%s\t%s\t%s\n' "$REVISION" "$DIGEST" "$NAME" >> "$TEMP"
done
mv "$TEMP" "$DIRECTORY/tool-revisions.tsv"
