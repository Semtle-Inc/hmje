#!/bin/sh
# Shared packaging defaults for POSIX vendoring and pin updates.
hmje_platform_default() {
    sed -n "s/^$1=//p" "$ROOT/tools/platform-libraries.defaults" | tr -d '\r'
}

# Resolve relative repository names at the single configurable hosting root.
hmje_platform_url() {
    case "$1" in
        *://*|git@*) printf '%s\n' "$1" ;;
        *)
            base=${HMJE_PLATFORM_BASE_URL:-$(hmje_platform_default base_url)}
            [ -n "$base" ] || {
                echo 'error: set HMJE_PLATFORM_BASE_URL to the library repository base URL, or provide pinned local sources with HMJE_PLATFORM_SOURCES (public hosting is not configured)' >&2
                return 1
            }
            printf '%s/%s\n' "${base%/}" "$1" ;;
    esac
}
