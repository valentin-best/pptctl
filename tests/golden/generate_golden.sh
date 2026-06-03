#!/usr/bin/env bash
# Regenerate all golden files from a built pptctl binary.
# Usage: ./tests/golden/generate_golden.sh <path-to-pptctl-binary>
# Example: ./tests/golden/generate_golden.sh ./build/macos-release/pptctl
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-pptctl-binary>" >&2
    exit 1
fi

BIN="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURES="$SCRIPT_DIR/../fixtures"
GOLDEN="$SCRIPT_DIR"

FIXTURES_LIST=(minimal three_slides images_deck notes_and_masters tables_deck charts_deck)

for fixture in "${FIXTURES_LIST[@]}"; do
    for fmt in json md txt; do
        outfile="$GOLDEN/${fixture}.${fmt}"
        "$BIN" extract --format "$fmt" "$FIXTURES/${fixture}.pptx" > "$outfile"
        # Normalize the machine-specific path in the "file" field.
        if [[ "$fmt" == "json" ]]; then
            sed -i.bak 's|"file": ".*"|"file": "<path>"|' "$outfile"
            rm -f "${outfile}.bak"
        fi
        echo "wrote $outfile"
    done
done
