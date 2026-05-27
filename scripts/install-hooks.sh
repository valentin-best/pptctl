#!/usr/bin/env bash
# Symlinks scripts/pre-commit into .git/hooks/.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
ln -sf "../../scripts/pre-commit" "$root/.git/hooks/pre-commit"
chmod +x "$root/scripts/pre-commit"
echo "pre-commit hook installed"
