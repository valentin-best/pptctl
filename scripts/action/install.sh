#!/usr/bin/env bash
# Put pptctl on PATH for the pptctl GitHub Action (action.yml).
#
# Downloads the release archive for this runner and checks it against the
# release's SHA256SUMS. When there is no archive for the runner, or the
# download fails (the token can't read the pptctl repository while it is
# private), builds pptctl from the Action's own source instead: slower, but it
# needs only CMake and a C++20 compiler (Linux and macOS runners have both).
#
# Environment, filled by action.yml:
#   PPTCTL_VERSION   release tag (v0.6.1); empty means the Action's own tag
#                    when it is a release tag (ACTION_REF), else the newest
#   ACTION_REF       the ref the workflow named in `uses: …@ref`
#   PPTCTL_REPO      repository the releases live in (valentin-best/pptctl)
#   GH_TOKEN         token for `gh release download`
#   ACTION_PATH      the Action's checkout, for the source build
# plus RUNNER_OS, RUNNER_ARCH, RUNNER_TEMP and GITHUB_PATH from the runner.

# Empty arrays expand as ${a[@]+"${a[@]}"}: bash 3.2 (macOS runners) treats
# "${a[@]}" of an empty array as unbound under set -u.
set -euo pipefail

if [ -z "${PPTCTL_VERSION:-}" ] && [[ ${ACTION_REF:-} =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    PPTCTL_VERSION=$ACTION_REF
fi

bin_dir=$RUNNER_TEMP/pptctl-bin
mkdir -p "$bin_dir"

case "$RUNNER_OS/$RUNNER_ARCH" in
    Linux/X64) target=linux-x86_64 ;;
    Linux/ARM64) target=linux-aarch64 ;;
    macOS/ARM64) target=macos-arm64 ;;
    Windows/X64) target=windows-x86_64 ;;
    *) target= ;;
esac

download() {
    local work args=()
    work=$(mktemp -d)
    [ -n "${PPTCTL_VERSION:-}" ] && args+=("$PPTCTL_VERSION")
    gh release download ${args[@]+"${args[@]}"} --repo "$PPTCTL_REPO" --dir "$work" \
        --pattern "pptctl-*-$target.*" --pattern SHA256SUMS || return 1

    (
        cd "$work"
        if command -v sha256sum > /dev/null; then
            sha256sum --check --ignore-missing SHA256SUMS
        else
            shasum -a 256 --check --ignore-missing SHA256SUMS
        fi
    )
    case $target in
        windows-*) (cd "$work" && 7z x -bso0 pptctl-*.zip) ;;
        *) tar -xzf "$work"/pptctl-*.tar.gz -C "$work" ;;
    esac
    cp "$work"/pptctl-*-"$target"/pptctl* "$bin_dir/"
}

build_from_source() {
    local build=$RUNNER_TEMP/pptctl-build generator=()
    command -v ninja > /dev/null && generator=(-G Ninja)
    cmake -S "$ACTION_PATH" -B "$build" ${generator[@]+"${generator[@]}"} -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --target pptctl --parallel
    cp "$build/pptctl" "$bin_dir/"
}

if [ -n "$target" ] && download; then
    echo "pptctl: installed the ${PPTCTL_VERSION:-newest} release for $target"
elif [ "$RUNNER_OS" != Windows ]; then
    echo "::notice title=pptctl::No release download for $RUNNER_OS/$RUNNER_ARCH; building from source"
    build_from_source
else
    echo "::error title=pptctl::Could not download pptctl for $RUNNER_OS/$RUNNER_ARCH"
    exit 1
fi

echo "$bin_dir" >> "$GITHUB_PATH"
"$bin_dir"/pptctl --version
