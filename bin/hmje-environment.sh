# Sourced by the native drivers after resolving their own symlinks.
# Installation payloads are immutable; all generated state lives outside them.
if [ -z "${HMJE_CACHE_HOME:-}" ]; then
    case "$(uname -s)" in
        Darwin) HMJE_CACHE_HOME=${HOME:?HOME is required}/Library/Caches/hmje ;;
        *) HMJE_CACHE_HOME=${XDG_CACHE_HOME:-${HOME:?HOME is required}/.cache}/hmje ;;
    esac
fi
case "$HMJE_CACHE_HOME" in
    /*) ;;
    *) HMJE_CACHE_HOME=$PWD/$HMJE_CACHE_HOME ;;
esac
export HMJE_CACHE_HOME
# Report the selected executable's provenance, never the launcher's checkout.
# Metadata is bound to the complete executable bytes; stale/copied metadata
# cannot make an overridden or modified tool claim another source revision.
hmje_tool_revision() (
    HMJE_VERSION_TOOL=$1
    HMJE_VERSION_LINKS=0
    while [ -L "$HMJE_VERSION_TOOL" ]; do
        HMJE_VERSION_LINKS=$((HMJE_VERSION_LINKS + 1))
        [ "$HMJE_VERSION_LINKS" -le 64 ] || { printf 'unknown (symlink loop)'; exit; }
        HMJE_VERSION_LINK=$(readlink "$HMJE_VERSION_TOOL")
        case "$HMJE_VERSION_LINK" in
            /*) HMJE_VERSION_TOOL=$HMJE_VERSION_LINK ;;
            *) HMJE_VERSION_TOOL=$(dirname -- "$HMJE_VERSION_TOOL")/$HMJE_VERSION_LINK ;;
        esac
    done
    [ -f "$HMJE_VERSION_TOOL" ] && [ -x "$HMJE_VERSION_TOOL" ] || {
        printf 'unknown (missing executable)'; exit;
    }
    if command -v shasum >/dev/null 2>&1; then
        HMJE_VERSION_DIGEST=$(shasum -a 256 < "$HMJE_VERSION_TOOL")
    elif command -v sha256sum >/dev/null 2>&1; then
        HMJE_VERSION_DIGEST=$(sha256sum < "$HMJE_VERSION_TOOL")
    else
        printf 'unknown (SHA-256 unavailable)'; exit;
    fi
    HMJE_VERSION_DIGEST=${HMJE_VERSION_DIGEST%% *}
    HMJE_VERSION_DIRECTORY=$(CDPATH= cd -- "$(dirname -- "$HMJE_VERSION_TOOL")" && pwd)
    if [ -f "$HMJE_VERSION_DIRECTORY/tool-revisions.tsv" ]; then
        while IFS="$(printf '\t')" read -r HMJE_VERSION_SOURCE HMJE_VERSION_HASH HMJE_VERSION_NAME; do
            case "$HMJE_VERSION_SOURCE" in ''|'#'*) continue ;; esac
            if [ "$HMJE_VERSION_HASH" = "$HMJE_VERSION_DIGEST" ] && [ -n "$HMJE_VERSION_NAME" ]; then
                printf '%s (sha256 %s)' "$HMJE_VERSION_SOURCE" "$HMJE_VERSION_DIGEST"
                exit
            fi
        done < "$HMJE_VERSION_DIRECTORY/tool-revisions.tsv"
    fi
    printf 'unknown (sha256 %s)' "$HMJE_VERSION_DIGEST"
)
hmje_validate_jobs() {
    case "$HMJE_JOBS" in
        ''|*[!0-9]*) echo 'error: jobs must be an integer from 1 to 64' >&2; exit 2 ;;
    esac
    HMJE_JOB_VALUE=$HMJE_JOBS
    if [ "${#HMJE_JOB_VALUE}" -gt 2 ] || [ "$HMJE_JOB_VALUE" -lt 1 ] || [ "$HMJE_JOB_VALUE" -gt 64 ]; then
        echo 'error: jobs must be an integer from 1 to 64' >&2; exit 2
    fi
}
if [ -d "$ROOT/libexec/bin" ]; then
    HMJE_TOOL_BIN=${HMJE_TOOL_BIN:-$ROOT/libexec/bin}
fi
# Shared-cache pruner lookup. Every driver that writes the default shared cache
# runs the same automatic check, whichever compiler it drives: an installed
# payload (libexec), a build-native output (HMJE_BOOTSTRAP_BIN/HMJE_TOOL_BIN or
# the directory of HMJE_FRONTEND), or a checkout's stage0 seed, which ships no
# pruner. For the last case the checkout's tools/cache-prune.c is compiled once
# per source revision into the cache's .tools directory. Prints nothing when no
# tool can be found or built; callers then skip maintenance.
hmje_cache_tool() {
    for HMJE_CACHE_CANDIDATE in "${HMJE_CACHE_TOOL:-}" "$ROOT/libexec/bin/hmjec-cache" \
        "${1:+$1/hmjec-cache}" "${HMJE_FRONTEND:+$(dirname -- "$HMJE_FRONTEND")/hmjec-cache}"; do
        if [ -n "$HMJE_CACHE_CANDIDATE" ] && [ -f "$HMJE_CACHE_CANDIDATE" ] && [ -x "$HMJE_CACHE_CANDIDATE" ]; then
            printf '%s\n' "$HMJE_CACHE_CANDIDATE"
            return 0
        fi
    done
    [ -f "$ROOT/tools/cache-prune.c" ] || return 0
    HMJE_CACHE_SUM=$(cksum < "$ROOT/tools/cache-prune.c" 2>/dev/null | tr ' ' '-') || return 0
    HMJE_CACHE_BUILT=$HMJE_CACHE_HOME/v1/.tools/hmjec-cache-$HMJE_CACHE_SUM-$(uname -m)
    if [ ! -x "$HMJE_CACHE_BUILT" ]; then
        mkdir -p "$HMJE_CACHE_HOME/v1/.tools" 2>/dev/null || return 0
        HMJE_CACHE_PARTIAL=$HMJE_CACHE_BUILT.tmp.$$
        if "${CC:-cc}" -std=c11 -O2 "$ROOT/tools/cache-prune.c" -o "$HMJE_CACHE_PARTIAL" >/dev/null 2>&1; then
            mv -f "$HMJE_CACHE_PARTIAL" "$HMJE_CACHE_BUILT" 2>/dev/null || { rm -f "$HMJE_CACHE_PARTIAL"; return 0; }
        else
            rm -f "$HMJE_CACHE_PARTIAL"
            return 0
        fi
    fi
    printf '%s\n' "$HMJE_CACHE_BUILT"
}

# A failed invocation must not leave a previous product looking freshly built.
# Never unlink special devices (--dump-deps uses /dev/null) or arbitrary folders.
hmje_failed_output_remove() {
    [ "$1" -ne 0 ] || return 0
    [ -n "$2" ] || return 0
    if [ -f "$2" ] || [ -L "$2" ]; then
        rm -f -- "$2"
    else
        case "$2" in *.app) rm -rf -- "$2" ;; esac
    fi
}
