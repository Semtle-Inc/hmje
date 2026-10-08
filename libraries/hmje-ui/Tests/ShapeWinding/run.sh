#!/bin/sh
# Verify signed crossings, contour orientation, negative coordinates and holes.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
UI_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
HMJEC=${HMJEC:-hmjec}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/hmje-shape-winding.XXXXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$WORK/Sources"
cp "$HERE/main.hmje" "$WORK/Sources/main.hmje"
cat > "$WORK/Project.hmjep" <<EOF
프로젝트 ShapeWinding의
    버전은 "0.1.0"이다.
의존 프로젝트 hmje-ui의
    로컬 경로는 "$UI_ROOT"이다.
모듈 ShapeWinding의
    원본 경로는 "Sources"이고,
    의존 라이브러리는 HmjeGraphics이다.
실행 파일 ShapeWinding의
    시작 모듈은 ShapeWinding이다.
EOF
HMJE_CACHE_HOME="$WORK/cache" HMJE_PARSE_CACHE="$WORK/parse" \
    "$HMJEC" --project "$WORK" --artifact ShapeWinding --target posix-host-exe -o "$WORK/ShapeWinding"
"$WORK/ShapeWinding" > "$WORK/result"
printf 'shape-winding: pass\n' > "$WORK/expected"
cmp "$WORK/result" "$WORK/expected"
cat "$WORK/result"
