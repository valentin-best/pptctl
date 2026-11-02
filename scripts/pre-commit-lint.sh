#!/usr/bin/env bash
# Entry point of the `pptctl-lint-fetch` pre-commit hook (.pre-commit-hooks.yaml):
# runs `pptctl lint "$@"` with the pptctl release that matches the hook's `rev`,
# downloading it on first use.
#
# The version is the one this checkout of pptctl declares in CMakeLists.txt, so
# pinning `rev: v0.7.0` runs pptctl 0.7.0 whatever else is installed. The
# release archive is checked against the release's SHA256SUMS and unpacked into
# ${XDG_CACHE_HOME:-~/.cache}/pptctl/<tag>-<platform>/.
#
# Environment:
#   PPTCTL_VERSION     release tag to use instead (v0.7.0)
#   PPTCTL_CACHE_DIR   where releases are unpacked (default above)
#   PPTCTL_REPO        repository the releases live in (valentin-best/pptctl)
#
# Downloads go through curl; if that fails (a private repository) and the
# GitHub CLI is installed, through `gh release download`, which uses its login.

# Empty arrays expand as ${a[@]+"${a[@]}"}: bash 3.2 (macOS) treats "${a[@]}"
# of an empty array as unbound under set -u.
set -euo pipefail

die() {
    echo "pptctl-lint-fetch: $*" >&2
    exit 1
}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=${PPTCTL_REPO:-valentin-best/pptctl}

tag=${PPTCTL_VERSION:-}
if [ -z "$tag" ]; then
    version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$here/../CMakeLists.txt" | head -n 1)
    [ -n "$version" ] || die "can't read the version from $here/../CMakeLists.txt"
    tag=v$version
fi
version=${tag#v}

case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) target=linux-x86_64 ;;
    Linux/aarch64 | Linux/arm64) target=linux-aarch64 ;;
    Darwin/arm64) target=macos-arm64 ;;
    MINGW*/x86_64 | MSYS*/x86_64 | CYGWIN*/x86_64) target=windows-x86_64 ;;
    *)
        die "no pptctl release for $(uname -s)/$(uname -m); install pptctl yourself" \
            "and use the pptctl-lint hook instead"
        ;;
esac
case $target in
    windows-*) archive=pptctl-$version-$target.zip exe=pptctl.exe ;;
    *) archive=pptctl-$version-$target.tar.gz exe=pptctl ;;
esac

cache=${PPTCTL_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/pptctl}
dest=$cache/$tag-$target
bin=$dest/$exe

fetch() { # file into the current directory
    curl -fsSL --retry 2 -o "$1" "https://github.com/$repo/releases/download/$tag/$1" 2> /dev/null ||
        { command -v gh > /dev/null &&
            gh release download "$tag" --repo "$repo" --pattern "$1" --clobber; }
}

install() {
    mkdir -p "$cache"
    local work
    work=$(mktemp -d "$cache/.download.XXXXXX")
    # shellcheck disable=SC2064  # expand $work now
    trap "rm -rf '$work'" EXIT
    (
        cd "$work"
        if ! fetch "$archive" || ! fetch SHA256SUMS; then
            die "could not download $archive from the $tag release of $repo"
        fi
        grep " $archive\$" SHA256SUMS > want || die "SHA256SUMS of $tag has no $archive"
        if command -v sha256sum > /dev/null; then
            sha256sum --check --quiet want
        else
            shasum -a 256 --check --quiet want
        fi || die "$archive does not match the release's SHA256SUMS"
        case $archive in
            *.zip)
                if command -v unzip > /dev/null; then
                    unzip -q "$archive"
                else
                    powershell -NoProfile -Command "Expand-Archive -Path '$archive' -DestinationPath ."
                fi
                ;;
            *) tar -xzf "$archive" ;;
        esac
        [ -f "pptctl-$version-$target/$exe" ] || die "$archive has no $exe"
    )
    # Another hook run may have finished first; either copy is the same file.
    mv "$work/pptctl-$version-$target" "$dest" 2> /dev/null || [ -x "$bin" ] ||
        die "could not install pptctl into $dest"
    rm -rf "$work"
    trap - EXIT
    echo "pptctl-lint-fetch: installed pptctl $version into $dest" >&2
}

[ -x "$bin" ] || install
exec "$bin" lint "$@"
