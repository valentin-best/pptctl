#!/bin/sh
# Print one version's section of CHANGELOG.md, without its heading, as the
# body of the GitHub Release:
#
#   scripts/release/notes.sh 0.6.0
#
# Fails when the changelog has no section for that version.

set -eu

notes=$(awk -v heading="## [$1]" '
    index($0, heading) == 1 { on = 1; next }
    on && /^## \[/ { exit }
    on && /^\[[^]]*\]: / { exit }
    on { print }
' CHANGELOG.md)

if [ -z "$(printf '%s' "$notes" | tr -d '[:space:]')" ]; then
    echo "CHANGELOG.md has no section for $1" >&2
    exit 1
fi
printf '%s\n' "$notes"
