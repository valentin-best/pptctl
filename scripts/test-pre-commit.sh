#!/usr/bin/env bash
# End-to-end test of the pre-commit hooks in .pre-commit-hooks.yaml: a scratch
# repo whose .pre-commit-config.yaml points at this checkout's HEAD commits a
# clean deck (must pass) and a deck with lint findings (must be blocked).
#
#   scripts/test-pre-commit.sh BUILD_DIR [HOOK...]
#
# BUILD_DIR holds the pptctl binary the `pptctl-lint` hook finds on PATH. HOOKs
# default to both: pptctl-lint and pptctl-lint-fetch. The fetch hook downloads
# the release named in CMakeLists.txt (gh needs a login, or GH_TOKEN, while the
# repository is private) into a scratch cache.
#
# Hooks come from the committed HEAD, so commit .pre-commit-hooks.yaml and
# scripts/pre-commit-lint.sh changes before running this.
#
# Environment: PRE_COMMIT, the pre-commit command (default: pre-commit, else
# `uvx pre-commit`).

set -euo pipefail

[ $# -ge 1 ] || {
    sed -n '6p' "$0" >&2
    exit 2
}
build=$(cd "$1" && pwd)
shift
hooks=("$@")
[ ${#hooks[@]} -gt 0 ] || hooks=(pptctl-lint pptctl-lint-fetch)

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fixtures=$root/tests/fixtures
# Git for Windows: pre-commit (native Python) needs D:/… rather than /d/….
root_url=$( (cd "$root" && pwd -W 2> /dev/null) || echo "$root")
rev=$(git -C "$root" rev-parse HEAD)

if [ -n "${PRE_COMMIT:-}" ]; then
    read -r -a pre_commit <<< "$PRE_COMMIT"
elif command -v pre-commit > /dev/null; then
    pre_commit=(pre-commit)
else
    pre_commit=(uvx pre-commit)
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export PATH="$build:$PATH"
export PPTCTL_CACHE_DIR=$scratch/pptctl-cache
export PRE_COMMIT_HOME=$scratch/pre-commit-cache
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# commit <expect: pass|block> <message> <fixture>…: copy the fixtures into the
# repo, stage and commit them, and check the hook let the commit through or not.
commit() {
    local expect=$1 msg=$2 rc=0
    shift 2
    for f in "$@"; do cp "$fixtures/$f" .; done
    git add -- "$@"
    git commit -q -m "$msg" > out.log 2>&1 || rc=$?
    if [ "$expect" = pass ] && [ $rc -ne 0 ]; then
        fail "$hook: '$msg' was blocked"
        cat out.log >&2
    elif [ "$expect" = block ] && [ $rc -eq 0 ]; then
        fail "$hook: '$msg' went through"
        cat out.log >&2
    else
        echo "ok: $hook: $msg ($expect)"
    fi
    [ $rc -eq 0 ] || git reset -q -- "$@"
}

for hook in "${hooks[@]}"; do
    repo=$scratch/$hook
    git init -q "$repo"
    cd "$repo"
    cat > .pre-commit-config.yaml << EOF
repos:
  - repo: $root_url
    rev: $rev
    hooks:
      - id: $hook
EOF
    git add .pre-commit-config.yaml
    git commit -q -m config
    "${pre_commit[@]}" install > /dev/null

    commit pass "clean deck" lint_clean.pptx
    commit block "deck with findings" lint_problems.pptx
    grep -q 'lint_problems.pptx:2: warning\[tiny-text\]' out.log ||
        fail "$hook: findings missing from the output"
    commit block "deck with integrity errors" integrity_problems.pptx
    "${pre_commit[@]}" run --all-files > out.log 2>&1 ||
        fail "$hook: run --all-files on the committed clean deck failed"
    if [ "$hook" = pptctl-lint ]; then
        # pre-commit passes every staged deck in one call. (The fetch hook runs
        # the released pptctl, which takes several files from v0.7.0 on.)
        git rm -q lint_clean.pptx
        git commit -q -m "drop clean deck"
        commit block "clean and bad deck together" lint_clean.pptx lint_problems.pptx
        if ! grep -q 'lint_clean.pptx: no issues found' out.log ||
            ! grep -q 'lint_problems.pptx: ' out.log; then
            fail "$hook: both decks should be reported"
        fi
    fi
    cd "$root"
done

if [ $failures -gt 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "all pre-commit hook tests passed"
