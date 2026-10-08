#!/bin/sh

# Owned scratch survives only when explicitly requested for debugging.
hmje_temp_remove() {
    if [ "${HMJE_KEEP_TEMP:-0}" = 1 ]; then
        for hmje_temp_path do
            [ "$hmje_temp_path" = -- ] || [ -z "$hmje_temp_path" ] || printf 'HMJE_KEEP_TEMP=1: %s\n' "$hmje_temp_path" >&2
        done
    else
        rm -rf "$@"
    fi
}
# Install an explicitly selected, already built native toolchain. No stage0 at runtime.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -ne 2 ]; then
    echo 'usage: sh tools/install-native.sh PREFIX NATIVE_BIN_DIRECTORY' >&2
    exit 2
fi
PREFIX=$1
NATIVE=$(CDPATH= cd -- "$2" && pwd)
case "$PREFIX" in /*) ;; *) PREFIX=$PWD/$PREFIX ;; esac
PAYLOAD=$PREFIX/lib/hmje
for tool in 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기 hmjec-cache; do
    [ -x "$NATIVE/$tool" ] || { echo "missing native tool: $NATIVE/$tool" >&2; exit 2; }
done
# Never merge versions or overwrite an existing user installation.
[ ! -e "$PAYLOAD" ] && [ ! -e "$PREFIX/bin/hmjec" ] && [ ! -L "$PREFIX/bin/hmjec" ] || {
    echo 'destination already contains hmje; select a fresh versioned prefix' >&2; exit 2;
}
mkdir -p "$PREFIX/lib" "$PREFIX/bin"
STAGING=$(mktemp -d "$PREFIX/lib/.hmje-install.XXXXXXXX")
trap 'if [ -n "$STAGING" ]; then hmje_temp_remove "$STAGING"; fi' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$STAGING/bin" "$STAGING/libexec/bin" "$STAGING/compiler/runtime" "$STAGING/compiler/Sources/준비"
cp "$ROOT/bin/hmjec" "$ROOT/bin/hmjec-selfhost" "$ROOT/bin/hmje-environment.sh" "$STAGING/bin/"
cp "$ROOT/bin/hmje-macos-signing" "$STAGING/bin/"
if [ "$(uname -s)" = Darwin ]; then
    [ -x "$NATIVE/hmje-macos-signing" ] || { echo "missing native tool: hmje-macos-signing" >&2; exit 2; }
    cp "$NATIVE/hmje-macos-signing" "$STAGING/libexec/bin/"
fi
for tool in 컴파일러 hmir을llvm으로 제품조정기 프로젝트조정기 hmjec-cache; do
    cp "$NATIVE/$tool" "$STAGING/libexec/bin/"
done
if [ -f "$NATIVE/tool-revisions.tsv" ]; then
    cp "$NATIVE/tool-revisions.tsv" "$STAGING/libexec/bin/"
fi
cp -R "$ROOT/library" "$ROOT/platforms" "$STAGING/"
# Public releases keep the complete tool/runtime and third-party notices.
for notice in LICENSE LICENSE-RUNTIME UNICODE-LICENSE.txt THIRD_PARTY_NOTICES.md SOURCES.md; do
    [ ! -f "$ROOT/$notice" ] || cp "$ROOT/$notice" "$STAGING/"
done
# Platform libraries (platform-libraries.lock) ship with this toolchain version.
# build-native.sh vendors them next to the binaries; an older build directory is
# completed here from the same lock (offline when the commits are available locally).
if [ -f "$NATIVE/platform-libraries/index.tsv" ]; then
    cp -R "$NATIVE/platform-libraries" "$STAGING/"
else
    sh "$ROOT/tools/vendor-platform-libraries.sh" "$STAGING"
fi
# Internal DB sources are maintained in-tree but only the internal profile ships them.
if grep -q '^# profile	public$' "$STAGING/platform-libraries/index.tsv"; then
    rm -rf "$STAGING/library/Internal"
fi
cp "$ROOT/platform-libraries.lock" "$STAGING/"
# An internal install (index.tsv `# profile internal`) also records the private lock; a
# public install never carries it (docs/platform-libraries.md §1.1).
if grep -q '^# profile	internal$' "$STAGING/platform-libraries/index.tsv" && [ -f "$ROOT/platform-libraries.internal.lock" ]; then
    cp "$ROOT/platform-libraries.internal.lock" "$STAGING/"
fi
# Installed library sources are reference material, not a place to edit.
find "$STAGING/platform-libraries" -type f -exec chmod a-w {} +
cp "$ROOT/compiler/runtime/host_app.c" "$ROOT/compiler/runtime/host_app_windows.c" \
   "$ROOT/compiler/runtime/host_scratch_os.c" "$ROOT/compiler/runtime/host_directory_windows.c" \
   "$ROOT/compiler/runtime/host_support.c" "$ROOT/compiler/runtime/host_support_windows.c" \
   "$ROOT/compiler/runtime/host_float_format.c" "$ROOT/compiler/runtime/host_compiler_common.c" \
   "$ROOT/compiler/runtime/host_compiler_posix.c" \
   "$STAGING/compiler/runtime/"
cp "$ROOT/compiler/runtime/host_paths.h" "$ROOT/compiler/runtime/host_paths_windows.c" \
   "$ROOT/compiler/runtime/host_process.h" "$ROOT/compiler/runtime/host_process_windows.c" \
   "$ROOT/compiler/runtime/host_files.h" "$ROOT/compiler/runtime/host_files_windows.c" \
   "$ROOT/compiler/runtime/host_sha256_windows.c" "$ROOT/compiler/runtime/host_workers_windows.c" \
   "$ROOT/compiler/runtime/windows_host_path.h" \
   "$STAGING/compiler/runtime/"
cp "$ROOT/compiler/Sources/준비/기본동적배열.hmje" "$STAGING/compiler/Sources/준비/"
if [ -f "$ROOT/SOURCE-REVISION" ]; then
    cp "$ROOT/SOURCE-REVISION" "$STAGING/"
elif [ -f "$NATIVE/SOURCE-REVISION" ]; then
    cp "$NATIVE/SOURCE-REVISION" "$STAGING/"
elif command -v git >/dev/null 2>&1; then
    git -C "$ROOT" describe --always --dirty > "$STAGING/SOURCE-REVISION"
else
    echo unknown > "$STAGING/SOURCE-REVISION"
fi
if command -v shasum >/dev/null 2>&1; then
    (cd "$STAGING/libexec/bin" && shasum -a 256 ./*) > "$STAGING/NATIVE-SHA256"
else
    (cd "$STAGING/libexec/bin" && sha256sum ./*) > "$STAGING/NATIVE-SHA256"
fi
mv "$STAGING" "$PAYLOAD"
STAGING=
ln -s ../lib/hmje/bin/hmjec "$PREFIX/bin/hmjec"
printf 'Installed %s/bin/hmjec (native binaries; no bootstrap dependency)\n' "$PREFIX"
# Warm this user's shared build cache with every platform library so that a new
# project's first build reuses parsed/lowered library code and object partitions.
# HMJE_INSTALL_WARM=0 skips it (CI, packaging for another user); failure is not fatal.
if [ "${HMJE_INSTALL_WARM:-1}" != 0 ]; then
    "$PREFIX/bin/hmjec" --warm-platform-libraries || true
fi
