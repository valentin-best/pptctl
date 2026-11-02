#!/usr/bin/env bash
# End-to-end test of docs/git-integration.md: applies its "Setup in one go"
# block to a throwaway global git config, then checks the three recipes in a
# scratch repo with two commits of a deck.
#
#   scripts/test-git-integration.sh BUILD_DIR
#
# BUILD_DIR holds the pptctl binary; the two git scripts come from scripts/,
# all three on PATH as a release install would put them.

set -euo pipefail

[ $# -eq 1 ] || {
    sed -n '6p' "$0" >&2
    exit 2
}
build=$(cd "$1" && pwd)
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fixtures=$root/tests/fixtures

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export PATH="$build:$root/scripts:$PATH"
export GIT_CONFIG_GLOBAL=$scratch/gitconfig GIT_CONFIG_NOSYSTEM=1
export TMPDIR=$scratch/tmp PPTCTL_OPEN=none
mkdir -p "$TMPDIR"
git config --global user.name test
git config --global user.email test@example.com

failures=0
check() { # description, then a command that must succeed
    local what=$1
    shift
    if "$@"; then echo "ok: $what"; else
        echo "FAIL: $what" >&2
        failures=$((failures + 1))
    fi
}

git init -q "$scratch/repo"
cd "$scratch/repo"

# The block from docs/git-integration.md, "Setup in one go", verbatim.
# shellcheck disable=SC2016  # $LOCAL etc. are for git to expand
{
    git config --global diff.pptx.textconv "pptctl extract --format txt"
    git config --global diff.pptx.command git-pptx-diff.sh
    git config --global difftool.pptx.cmd 'git-pptx-difftool.sh "$LOCAL" "$REMOTE" "$MERGED"'
    echo '*.pptx diff=pptx' >> .gitattributes
}

cp "$fixtures/diff_base.pptx" deck.pptx
echo notes > notes.md
git add . && git commit -q -m v1
cp "$fixtures/diff_text_edit.pptx" deck.pptx
echo more >> notes.md
git commit -q -am v2

# Recipe 2: git diff runs the driver, a semantic diff under the repo path.
git diff HEAD~1 HEAD -- deck.pptx > driver.out
check "driver: header names the repo path" grep -q '^--- a/deck.pptx' driver.out
check "driver: slide hunk" grep -q '^@@ slide [0-9].*changed' driver.out

# Recipe 1: git log -p (no --ext-diff) falls back to textconv.
git log -p -1 --format=%s -- deck.pptx > textconv.out
check "textconv: git log -p shows slide text" grep -q '^+  Review of this quarter$' textconv.out

# Recipe 3: difftool writes an HTML page labelled with the repo path. With
# diff.pptx.command set too, the driver has to hand over to the difftool.
git difftool -y -t pptx HEAD~1 HEAD -- '*.pptx' > difftool.out
page=$(sed -n 's/^deck.pptx: //p' difftool.out)
check "difftool: printed the page path" test -s "$page"
check "difftool: page labels a/deck.pptx" grep -q '<code class="path">a/deck.pptx</code>' "$page"
check "difftool: page labels b/deck.pptx" grep -q '<code class="path">b/deck.pptx</code>' "$page"

cp "$fixtures/minimal.pptx" added.pptx
git add added.pptx
git difftool -y -t pptx --cached -- '*.pptx' > added.out
check "difftool: added deck gets one line" grep -qx 'pptctl diff: new file added.pptx' added.out
git diff --cached -- added.pptx > added-driver.out
check "driver: added deck gets one line" grep -qx 'pptctl diff: new file added.pptx' added-driver.out

if [ $failures -gt 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "all git integration tests passed"
