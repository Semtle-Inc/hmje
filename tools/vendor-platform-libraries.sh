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
# Vendor the pinned platform libraries (platform-libraries.lock) into
# OUTPUT_DIRECTORY/platform-libraries. The result is what install-native.sh copies
# into lib/hmje/platform-libraries; installation itself never needs the network.
#
# Sources are exact commits exported with `git archive`, never working trees, so
# a sibling checkout on a feature branch or with local edits cannot leak in.
# Commit lookup order for each repository (basename of its URL, e.g. hmje-ui):
#   1. every directory in HMJE_PLATFORM_SOURCES (colon separated): DIR/<repo> or DIR/<repo>.git
#   2. the toolchain checkout's siblings (../<repo>), the usual development layout
#   3. a bare mirror in ${HMJE_CACHE_HOME}/platform-sources/<repo>.git, fetched from the
#      lock URL when the commit is missing (HMJE_PLATFORM_OFFLINE=1 forbids fetching)
#
# Profiles (docs/platform-libraries.md §1.1): HMJE_PLATFORM_PROFILE=public vendors only
# platform-libraries.lock — the set a public toolchain may ship. internal (the default
# in this internal repository) adds platform-libraries.internal.lock, the private libraries (for
# example the Semtle design system). index.tsv records the profile.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: sh tools/vendor-platform-libraries.sh OUTPUT_DIRECTORY' >&2; exit 2; }
LOCK=${HMJE_PLATFORM_LOCK:-$ROOT/platform-libraries.lock}
INTERNAL_LOCK=${HMJE_PLATFORM_INTERNAL_LOCK:-$ROOT/platform-libraries.internal.lock}
. "$ROOT/tools/platform-library-url.sh"
PROFILE=${HMJE_PLATFORM_PROFILE:-$(hmje_platform_default profile)}
[ -f "$LOCK" ] || { echo "missing platform library lock: $LOCK" >&2; exit 2; }
case "$PROFILE" in
    public|internal) ;;
    *) echo "error: HMJE_PLATFORM_PROFILE must be public or internal, not $PROFILE" >&2; exit 2 ;;
esac
mkdir -p "$1"
OUTPUT=$(CDPATH= cd -- "$1" && pwd)/platform-libraries
if [ -z "${HMJE_CACHE_HOME:-}" ]; then
    case "$(uname -s)" in
        Darwin) HMJE_CACHE_HOME=$HOME/Library/Caches/hmje ;;
        *) HMJE_CACHE_HOME=${XDG_CACHE_HOME:-$HOME/.cache}/hmje ;;
    esac
fi
MIRRORS=$HMJE_CACHE_HOME/platform-sources
STAGING=$(mktemp -d "${TMPDIR:-/tmp}/hmje-platform.XXXXXXXX")
trap 'hmje_temp_remove "$STAGING"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$STAGING/out"

has_commit() {
    git -C "$1" rev-parse -q --verify "$2^{commit}" >/dev/null 2>&1
}

find_source() { # repo commit url -> prints a git directory containing commit
    repo=$1 commit=$2 url=$3
    # Internal in-tree projects are pinned to a committed toolchain tree.
    if [ "$repo" = hmje-toolchain ] && has_commit "$ROOT" "$commit"; then
        printf '%s\n' "$ROOT"; return 0
    fi
    old_ifs=$IFS; IFS=:
    for directory in ${HMJE_PLATFORM_SOURCES:-} "$ROOT/.."; do
        IFS=$old_ifs
        [ -n "$directory" ] || continue
        for candidate in "$directory/$repo" "$directory/$repo.git"; do
            if [ -e "$candidate" ] && has_commit "$candidate" "$commit"; then
                printf '%s\n' "$candidate"; return 0
            fi
        done
    done
    IFS=$old_ifs
    mirror=$MIRRORS/$repo.git
    if [ -d "$mirror" ] && has_commit "$mirror" "$commit"; then
        printf '%s\n' "$mirror"; return 0
    fi
    if [ "${HMJE_PLATFORM_OFFLINE:-0}" = 1 ]; then
        echo "error: $repo $commit is not available offline (set HMJE_PLATFORM_SOURCES)" >&2
        return 1
    fi
    url=$(hmje_platform_url "$url") || return 1
    mkdir -p "$MIRRORS"
    [ -d "$mirror" ] || git init -q --bare "$mirror"
    echo "fetching $url for $repo $commit" >&2
    git -C "$mirror" fetch -q "$url" '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' >&2 || return 1
    has_commit "$mirror" "$commit" || { echo "error: $url has no commit $commit" >&2; return 1; }
    printf '%s\n' "$mirror"
}

# When including internal libraries, validate that the two sets are disjoint.
if [ "$PROFILE" = internal ]; then
    [ -f "$INTERNAL_LOCK" ] || { echo "error: internal profile requires $INTERNAL_LOCK" >&2; exit 2; }
    for internal in $(awk '$1 !~ /^#/ && NF { print $1 }' "$INTERNAL_LOCK"); do
        if awk -v n="$internal" '$1 == n { found = 1 } END { exit !found }' "$LOCK"; then
            echo "error: internal platform library $internal is also in the public lock $LOCK" >&2; exit 2
        fi
    done
fi
cp "$LOCK" "$STAGING/lock"
if [ "$PROFILE" = internal ] && [ -f "$INTERNAL_LOCK" ]; then
    cat "$INTERNAL_LOCK" >> "$STAGING/lock"
fi
printf '# profile\t%s\n' "$PROFILE" > "$STAGING/out/index.tsv"
printf '# name\tcommit\trepository\tpath\n' >> "$STAGING/out/index.tsv"
NAMES=
MONOREPO=$ROOT/libraries
# A public monorepo already contains the reviewed pinned exports. Compare its
# complete provenance index to the selected lock before using local source.
if [ "$PROFILE" = public ] && [ -d "$MONOREPO" ]; then
    awk '$1 !~ /^#/ && NF { print $1 "\t" $3 "\t" $2 "\t" $4 }' "$STAGING/lock" > "$STAGING/expected-index"
    awk '$1 !~ /^#/ && NF { print }' "$MONOREPO/index.tsv" > "$STAGING/actual-index"
    cmp -s "$STAGING/expected-index" "$STAGING/actual-index" || {
        echo 'error: monorepo library index does not match the selected public lock' >&2; exit 2;
    }
fi
while read -r name url commit path rest; do
    case "$name" in ''|'#'*) continue ;; esac
    [ -n "$path" ] && [ -z "$rest" ] || { echo "error: malformed lock line for $name" >&2; exit 2; }
    case "$commit" in *[!0-9a-f]*) echo "error: $name: commit must be a full hexadecimal hash" >&2; exit 2 ;; esac
    [ "${#commit}" -eq 40 ] || { echo "error: $name: commit must be a full 40-digit hash" >&2; exit 2; }
    case " $NAMES " in *" $name "*) echo "error: duplicate platform library $name" >&2; exit 2 ;; esac
    case "$name" in hmje-library|*/*|.*) echo "error: invalid platform library name $name" >&2; exit 2 ;; esac
    NAMES="$NAMES $name"
    repo=$(basename -- "$url" .git)
    extract=$STAGING/extract-$name
    mkdir -p "$extract"
    if [ "$PROFILE" = public ] && [ -d "$MONOREPO" ]; then
        [ "$path" = . ] && [ -f "$MONOREPO/$name/Project.hmjep" ] || {
            echo "error: incomplete monorepo library: $name" >&2; exit 2;
        }
        cp -R "$MONOREPO/$name/." "$extract/"
        tree=$extract
    elif [ "$path" = . ]; then
        source=$(find_source "$repo" "$commit" "$url")
        git -C "$source" archive --format=tar "$commit" | tar -x -C "$extract"
        tree=$extract
    else
        source=$(find_source "$repo" "$commit" "$url")
        git -C "$source" archive --format=tar "$commit" -- "$path" | tar -x -C "$extract"
        tree=$extract/$path
    fi
    [ -f "$tree/Project.hmjep" ] || { echo "error: $name has no Project.hmjep at $path" >&2; exit 2; }
    # Tests, examples, docs and development links are not part of the library.
    for pruned in Tests tests docs Examples examples scratch_check Dependencies \
                  .gitlab-ci.yml .gitignore .gitattributes .github; do
        rm -rf "${tree:?}/$pruned"
    done
    find "$tree" -type l -exec rm -f {} +
    mv "$tree" "$STAGING/out/$name"
    rm -rf "$extract"
    printf '%s\t%s\t%s\t%s\n' "$name" "$commit" "$url" "$path" >> "$STAGING/out/index.tsv"
    echo "vendored $name $commit" >&2
done < "$STAGING/lock"
rm -rf "$OUTPUT"
mv "$STAGING/out" "$OUTPUT"
echo "platform libraries ($PROFILE): $OUTPUT" >&2
