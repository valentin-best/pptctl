#!/usr/bin/env bash
# Regenerate all golden files (extract + lint + diff + optimize + merge) from a built pptctl binary.
# Usage: ./tests/golden/generate_golden.sh <path-to-pptctl-binary>
# Example: ./tests/golden/generate_golden.sh ./build/macos-release/pptctl
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-pptctl-binary>" >&2
    exit 1
fi

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"  # absolute: the merge goldens cd
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
               bloat_deck lint_problems lint_clean integrity_problems)

for fixture in "${LINT_FIXTURES[@]}"; do
    outfile="$GOLDEN/${fixture}.lint.json"
    # lint exits 0 without --strict even when it reports findings.
    "$BIN" lint --json "$FIXTURES/${fixture}.pptx" > "$outfile"
    sed -i.bak 's|"file": ".*"|"file": "<path>"|' "$outfile"
    rm -f "${outfile}.bak"
    echo "wrote $outfile"
done

# Diff goldens: one <a>__<b>.diff.{txt,json,html} per pair. Besides one
# real-PowerPoint pair and one self-diff, diff_base.pptx is diffed against each
# of its one-edit variants (#75; built by build_diff_deck in
# ../fixtures/generate_fixtures.py) and against itself, so every op family has
# a pinned rendering in every format:
#   diff_text_edit     word changed, bullet added, bullet deleted, run bolded, notes
#   diff_reorder       slides 2 and 4 swapped (moved, no nested ops)
#   diff_slide_added   one slide added at position 3
#   diff_slide_removed slide 4 removed
#   diff_shape_edit    rectangle moved + resized + recolored; a 1 EMU nudge elsewhere
#   diff_table_edit    table cell edited, row added (table ops land with #74)
# `diff` exits 1 when the decks differ, so only 2 (error) aborts. Input paths
# are normalized in each format the same way the test does: `---`/`+++` header
# lines, the `a`/`b` JSON fields, and the `<code class="path">` elements.
DIFF_PAIRS=("minimal three_slides" "three_slides three_slides"
            "diff_base diff_base" "diff_base diff_text_edit" "diff_base diff_reorder"
            "diff_base diff_slide_added" "diff_base diff_slide_removed"
            "diff_base diff_shape_edit" "diff_base diff_table_edit")

for pair in "${DIFF_PAIRS[@]}"; do
    read -r a b <<< "$pair"
    for fmt in unified json html; do
        case "$fmt" in
            unified) ext=txt ;;
            *) ext=$fmt ;;
        esac
        outfile="$GOLDEN/${a}__${b}.diff.${ext}"
        rc=0
        "$BIN" diff --no-color --format "$fmt" "$FIXTURES/${a}.pptx" "$FIXTURES/${b}.pptx" > "$outfile" || rc=$?
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

# Optimize goldens (#105): one <fixture>.optimize.json per fixture, the
# `optimize --dry-run --json` report. Besides the input path, every byte count
# that depends on the zlib in use (the system zlib on macOS and Linux, zlib-ng
# on Windows) becomes "<bytes>": the output size and both sizes of every action
# that isn't a removal. Removals keep their (uncompressed, deterministic) part
# sizes. The test applies the same rewrite, and checks the blanked counts as
# invariants instead (output smaller than input, rewritten parts smaller).
if ! command -v jq >/dev/null; then
    echo "error: the optimize goldens need jq" >&2
    exit 1
fi
OPTIMIZE_FIXTURES=(minimal notes_and_masters bloat_deck crop_deck optimize_deck)

for fixture in "${OPTIMIZE_FIXTURES[@]}"; do
    outfile="$GOLDEN/${fixture}.optimize.json"
    "$BIN" optimize --dry-run --json "$FIXTURES/${fixture}.pptx" | jq '
        .input = "<path>"
        | .output_bytes = "<bytes>"
        | .passes[].actions[] |= (
            if .kind == "removed" then . else .bytes_before = "<bytes>" | .bytes_after = "<bytes>" end)
    ' > "$outfile"
    echo "wrote $outfile"
done

# Merge goldens (#140): per case, merge/<case>.merge.json (the `merge --json`
# report: input paths as file names, the output path and its zlib-dependent
# size as placeholders) and merge/<case>.parts.txt (each output part's
# uncompressed size and name, in ZIP order). Run from the fixtures directory
# so the inputs are named by file name alone.
MERGE_CASES=("a_b:merge_a merge_b"
             "a_c:merge_a merge_c"
             "a_b_c:merge_a merge_b merge_c"
             "a_b_c_theme2:merge_a merge_b merge_c:--theme=2")
mkdir -p "$GOLDEN/merge"
merge_tmp="$(mktemp -d)"
trap 'rm -rf "$merge_tmp"' EXIT

for spec in "${MERGE_CASES[@]}"; do
    IFS=: read -r name decks flags <<< "$spec"
    inputs=()
    for deck in $decks; do
        inputs+=("${deck}.pptx")
    done
    out="$merge_tmp/${name}.pptx"
    # shellcheck disable=SC2086 # $flags is empty or one flag
    (cd "$FIXTURES" && "$BIN" merge "${inputs[@]}" $flags -o "$out" --json) |
        jq '.output = "<path>" | .output_bytes = "<bytes>"' > "$GOLDEN/merge/${name}.merge.json"
    unzip -l "$out" | awk 'NF >= 4 && $1 ~ /^[0-9]+$/ { print $1, $4 }' \
        > "$GOLDEN/merge/${name}.parts.txt"
    echo "wrote $GOLDEN/merge/${name}.merge.json, ${name}.parts.txt"
done
