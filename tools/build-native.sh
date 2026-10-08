#!/bin/sh
# Build distributable native tools using the selected seed, not a runtime dependency.
#
# HMJE_NATIVE_ARCH selects the tools' architecture (default: host):
#   host | aarch64 | x86_64   thin tools for that architecture
#   universal2                macOS only: arm64 + x86_64 tools merged with lipo,
#                             one install for Apple Silicon and Intel Macs
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: sh tools/build-native.sh OUTPUT_DIRECTORY' >&2; exit 2; }
case "$(uname -m)" in
    arm64|aarch64) HOST_ARCH=aarch64 ;;
    x86_64|amd64) HOST_ARCH=x86_64 ;;
    *) echo "unsupported build host architecture: $(uname -m)" >&2; exit 2 ;;
esac
NATIVE_ARCH=${HMJE_NATIVE_ARCH:-host}
case "$NATIVE_ARCH" in
    host) NATIVE_ARCH=$HOST_ARCH ;;
    arm64) NATIVE_ARCH=aarch64 ;;
    aarch64|x86_64) ;;
    universal|universal2)
        NATIVE_ARCH=universal2
        [ "$(uname -s)" = Darwin ] || { echo 'HMJE_NATIVE_ARCH=universal2 needs a Darwin host' >&2; exit 2; } ;;
    *) echo "unsupported HMJE_NATIVE_ARCH: $NATIVE_ARCH" >&2; exit 2 ;;
esac
if [ "$NATIVE_ARCH" != "$HOST_ARCH" ] && [ "$NATIVE_ARCH" != universal2 ] && [ "$(uname -s)" != Darwin ]; then
    echo "cross-architecture native tools need a Darwin host: $NATIVE_ARCH" >&2; exit 2
fi
if [ "$(uname -s)" = Darwin ]; then
    # Distributable tools must run on the oldest supported macOS, not carry the build
    # Mac's SDK version (26/27). clang and ld honour MACOSX_DEPLOYMENT_TARGET for every
    # object the seed compiles without an explicit -mmacosx-version-min.
    MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-14.0}
    export MACOSX_DEPLOYMENT_TARGET
    # The seed's link-object cache key does not include that variable, so objects cached
    # by earlier host-SDK builds would be reused. Keep these builds in their own cache
    # (unless the caller chose one).
    if [ -z "${HMJE_CACHE_HOME:-}" ]; then
        HMJE_CACHE_HOME=${HOME:?HOME is required}/Library/Caches/hmje-native-tools-macos$MACOSX_DEPLOYMENT_TARGET
        export HMJE_CACHE_HOME
    fi
fi
mkdir -p "$1"
NATIVE_OUTPUT=$(CDPATH= cd -- "$1" && pwd)
# The pinned platform libraries are part of the distributable toolchain. Vendor
# them first so a missing commit fails before the long native build.
sh "$ROOT/tools/vendor-platform-libraries.sh" "$NATIVE_OUTPUT"

# build_tools DIR ARCH: the five HMJE tools for ARCH into DIR. The seed backend can
# predate the compiler's current HMIR address ABI, so every later tool uses a
# just-built backend that runs on this host ($HOST_BACKEND once it exists).
HOST_BACKEND=
build_tools() {
    DIR=$1
    ARCH=$2
    mkdir -p "$DIR"
    set --
    [ "$ARCH" = "$HOST_ARCH" ] || set -- --target-arch "$ARCH"
    for artifact in hmir을llvm으로 제품조정기 프로젝트조정기 컴파일러; do
        if [ "$artifact" = hmir을llvm으로 ] && [ -z "$HOST_BACKEND" ]; then
            "$ROOT/bin/hmjec" --project "$ROOT/compiler" --artifact "$artifact" \
                --target posix-host-exe "$@" -o "$DIR/$artifact"
            # A cross build's own backend cannot run here (without Rosetta),
            # so it is rebuilt for the host below and used instead.
            [ "$ARCH" != "$HOST_ARCH" ] || HOST_BACKEND=$DIR/hmir을llvm으로
        else
            HMJE_BACKEND="${HOST_BACKEND:-$DIR/hmir을llvm으로}" \
                "$ROOT/bin/hmjec" --project "$ROOT/compiler" --artifact "$artifact" \
                --target posix-host-exe "$@" -o "$DIR/$artifact"
        fi
    done
}
# Released seeds may still select a window runtime for compiler projects.
# Build a host coordinator first, then use its current runtime selection for
# every distributable tool (including the backend and coordinator themselves).
BOOTSTRAP_TOOLS=$NATIVE_OUTPUT/.bootstrap-tools
mkdir -p "$BOOTSTRAP_TOOLS"
"$ROOT/bin/hmjec" --project "$ROOT/compiler" --artifact hmir을llvm으로 \
    --target posix-host-exe -o "$BOOTSTRAP_TOOLS/hmir을llvm으로"
HMJE_BACKEND=$BOOTSTRAP_TOOLS/hmir을llvm으로 \
    "$ROOT/bin/hmjec" --project "$ROOT/compiler" --artifact 제품조정기 \
        --target posix-host-exe -o "$BOOTSTRAP_TOOLS/제품조정기"
HMJE_COORDINATOR=$BOOTSTRAP_TOOLS/제품조정기
export HMJE_COORDINATOR
HOST_BACKEND=$BOOTSTRAP_TOOLS/hmir을llvm으로

if [ "$NATIVE_ARCH" = universal2 ]; then
    rm -rf "$NATIVE_OUTPUT/.arch-aarch64" "$NATIVE_OUTPUT/.arch-x86_64"
    if [ "$HOST_ARCH" = aarch64 ]; then OTHER_ARCH=x86_64; else OTHER_ARCH=aarch64; fi
    build_tools "$NATIVE_OUTPUT/.arch-$HOST_ARCH" "$HOST_ARCH"
    build_tools "$NATIVE_OUTPUT/.arch-$OTHER_ARCH" "$OTHER_ARCH"
    for tool in 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기; do
        # Each thin slice keeps its own deterministic ad-hoc signature.
        /usr/bin/lipo -create "$NATIVE_OUTPUT/.arch-aarch64/$tool" "$NATIVE_OUTPUT/.arch-x86_64/$tool" \
            -output "$NATIVE_OUTPUT/$tool"
        /usr/bin/codesign --verify --strict "$NATIVE_OUTPUT/$tool"
    done
    rm -rf "$NATIVE_OUTPUT/.arch-aarch64" "$NATIVE_OUTPUT/.arch-x86_64"
    # Same deployment target as the five HMJE tools above (MACOSX_DEPLOYMENT_TARGET, default 14.0).
    "${CC:-cc}" -arch arm64 -arch x86_64 -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -std=c11 -O2 -Wall -Wextra -Werror \
        "$ROOT/tools/cache-prune.c" -o "$NATIVE_OUTPUT/hmjec-cache"
    # Only the arm64 slice is linker-signed; sign both like the other tools.
    /usr/bin/codesign --force --sign - --timestamp=none "$NATIVE_OUTPUT/hmjec-cache"
else
    if [ "$NATIVE_ARCH" != "$HOST_ARCH" ]; then
        rm -rf "$NATIVE_OUTPUT/.host-backend"
        mkdir -p "$NATIVE_OUTPUT/.host-backend"
        "$ROOT/bin/hmjec" --project "$ROOT/compiler" --artifact hmir을llvm으로 \
            --target posix-host-exe -o "$NATIVE_OUTPUT/.host-backend/hmir을llvm으로"
        HOST_BACKEND=$NATIVE_OUTPUT/.host-backend/hmir을llvm으로
    fi
    build_tools "$NATIVE_OUTPUT" "$NATIVE_ARCH"
    rm -rf "$NATIVE_OUTPUT/.host-backend"
    if [ "$NATIVE_ARCH" = "$HOST_ARCH" ] && [ "$(uname -s)" != Darwin ]; then
        "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror "$ROOT/tools/cache-prune.c" -o "$NATIVE_OUTPUT/hmjec-cache"
    elif [ "$NATIVE_ARCH" = "$HOST_ARCH" ]; then
        # Name the slice: Xcode's cc shim runs arm64 even inside a Rosetta shell.
        "${CC:-cc}" -arch "$(uname -m)" -std=c11 -O2 -Wall -Wextra -Werror "$ROOT/tools/cache-prune.c" -o "$NATIVE_OUTPUT/hmjec-cache"
    else
        [ "$NATIVE_ARCH" = aarch64 ] && CACHE_ARCH=arm64 || CACHE_ARCH=x86_64
        "${CC:-cc}" -arch "$CACHE_ARCH" -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -std=c11 -O2 -Wall -Wextra -Werror \
            "$ROOT/tools/cache-prune.c" -o "$NATIVE_OUTPUT/hmjec-cache"
    fi
fi
if [ "$(uname -s)" = Darwin ]; then
    set -- -arch "$(uname -m)"
    case "$NATIVE_ARCH" in
        universal2) set -- -arch arm64 -arch x86_64 ;;
        aarch64) set -- -arch arm64 ;;
        x86_64) set -- -arch x86_64 ;;
    esac
    /usr/bin/clang "$@" -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -fobjc-arc -O2 -Wall -Wextra -Werror \
        -framework Foundation "$ROOT/tools/macos-signing.m" -o "$NATIVE_OUTPUT/hmje-macos-signing"
    /usr/bin/codesign --force --sign - --timestamp=none "$NATIVE_OUTPUT/hmje-macos-signing"
fi
if command -v shasum >/dev/null 2>&1; then
    (cd "$NATIVE_OUTPUT" && shasum -a 256 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기 hmjec-cache) > "$NATIVE_OUTPUT/manifest.sha256"
else
    (cd "$NATIVE_OUTPUT" && sha256sum 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기 hmjec-cache) > "$NATIVE_OUTPUT/manifest.sha256"
fi

if [ -f "$NATIVE_OUTPUT/hmje-macos-signing" ]; then
    (cd "$NATIVE_OUTPUT" && shasum -a 256 hmje-macos-signing) >> "$NATIVE_OUTPUT/manifest.sha256"
fi

rm -rf "$BOOTSTRAP_TOOLS"
sh "$ROOT/tools/write-tool-revisions.sh" "$NATIVE_OUTPUT"
