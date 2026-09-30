#!/usr/bin/env bash
# Regenerate all golden files (extract + lint + diff) from a built pptctl binary.
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

# Lint goldens: one <fixture>.lint.json per fixture, path normalized the same way.
LINT_FIXTURES=(minimal three_slides images_deck notes_and_masters tables_deck charts_deck
               bloat_deck lint_problems lint_clean)

for fixture in "${LINT_FIXTURES[@]}"; do
    outfile="$GOLDEN/${fixture}.lint.json"
    # lint exits 0 without --strict even when it reports findings.
    "$BIN" lint --json "$FIXTURES/${fixture}.pptx" > "$outfile"
    sed -i.bak 's|"file": ".*"|"file": "<path>"|' "$outfile"
    rm -f "${outfile}.bak"
    echo "wrote $outfile"
done

# Diff goldens: one <a>__<b>.diff.{txt,json,html} per pair, for one differing
# pair and one self-diff. `diff` exits 1 when the decks differ, so only 2
# (error) aborts. Input paths are normalized in each format the same way the
# test does: `---`/`+++` header lines, the `a`/`b` JSON fields, and the
# `<code class="path">` elements.
DIFF_PAIRS=("minimal three_slides" "three_slides three_slides")

for pair in "${DIFF_PAIRS[@]}"; do
    read -r a b <<< "$pair"
    for fmt in unified json html; do
        case "$fmt" in
            unified) ext=txt ;;
            *) ext=$fmt ;;
        esac
        outfile="$GOLDEN/${a}__${b}.diff.${ext}"
        rc=0
        "$BIN" diff --format "$fmt" "$FIXTURES/${a}.pptx" "$FIXTURES/${b}.pptx" > "$outfile" || rc=$?
        if [[ $rc -gt 1 ]]; then
            echo "error: diff exited $rc for $a vs $b" >&2
            exit 1
        fi
        sed -i.bak \
            -e 's|^--- .*|--- <path>|' \
            -e 's|^+++ .*|+++ <path>|' \
            -e 's|"\([ab]\)": ".*"|"\1": "<path>"|' \
            -e 's|<code class="path">[^<]*</code>|<code class="path">\&lt;path\&gt;</code>|g' \
            "$outfile"
        rm -f "${outfile}.bak"
        echo "wrote $outfile"
    done
done
