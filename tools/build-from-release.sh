#!/bin/sh
# OS packaging boundary: use an explicitly selected release as the initial compiler.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -eq 2 ] || { echo 'usage: sh tools/build-from-release.sh RELEASE_NATIVE_BIN OUTPUT_DIRECTORY' >&2; exit 2; }
HMJE_BOOTSTRAP_BIN=$(CDPATH= cd -- "$1" && pwd)
unset HMJE_TOOL_BIN HMJE_FRONTEND HMJE_BACKEND HMJE_COORDINATOR HMJE_PROJECT_COORDINATOR
unset HMJE_TOOLCHAIN_ROOT HMJE_COMPILER_ROOT HMJE_LIBRARY_ROOT HMJE_HOST_SUPPORT
for tool in 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기; do
    [ -x "$HMJE_BOOTSTRAP_BIN/$tool" ] || { echo "missing release tool: $tool" >&2; exit 2; }
done
export HMJE_BOOTSTRAP_BIN
HMJE_PLATFORM_PROFILE=public
HMJE_PLATFORM_OFFLINE=1
export HMJE_PLATFORM_PROFILE HMJE_PLATFORM_OFFLINE
sh "$ROOT/tools/build-native.sh" "$2"
