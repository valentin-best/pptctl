#!/usr/bin/env bash
# Validate `pptctl merge` on real-world decks from varied producers (#142).
#
# Merges the given decks, in order, twice: keeping each deck's formatting
# (the default) and restyled onto deck 1 (--theme=1). For each output:
#   1. merge --json, timed              -> slides, parts added, dedupe, size
#   2. lint --strict --only integrity   -> must be clean
# and for the default output, per input:
#   3. diff --format json input output  -> every input slide pairs with its
#                                          slot in the output, unchanged
# Under --theme, slides that the diff reports as changed are counted, not
# failed: inheriting a new layout is the point of the option.
#
# Usage:
#   scripts/validate_merge.sh DECK.pptx DECK.pptx [DECK.pptx …] [--bin PATH] [--work DIR]
#
#   --bin PATH  pptctl binary to test (default: build/macos-release/pptctl)
#   --work DIR  where outputs and reports go (default: tests/real_world/.work-merge,
#               gitignored)
#
# Needs jq. Exits non-zero on a crash or any failed check.

set -u

bin="build/macos-release/pptctl"
work="tests/real_world/.work-merge"
decks=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin) bin="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) decks+=("$1"); shift ;;
    esac
done

if [[ ! -x "$bin" ]]; then
    echo "error: binary not found: $bin (build first with ./scripts/build.sh)" >&2
    exit 2
fi
if [[ ${#decks[@]} -lt 2 ]]; then
    echo "usage: $0 DECK.pptx DECK.pptx [DECK.pptx …] [--bin PATH] [--work DIR]" >&2
    exit 2
fi
if ! command -v jq >/dev/null; then
    echo "error: validate_merge.sh needs jq" >&2
    exit 2
fi
mkdir -p "$work"
failures=0

fail() {
    echo "  FAIL: $*"
    failures=$((failures + 1))
}

slides_of() {
    "$bin" info --json "$1" | jq '.slides'
}

# merge_and_check NAME [FLAGS...]: merge every deck into $work/NAME.pptx and
# check the output's integrity. Prints the report table.
merge_and_check() {
    local name="$1"; shift
    local out="$work/$name.pptx" report="$work/$name.merge.json"
    local start end rc
    start=$(date +%s.%N 2>/dev/null || date +%s)
    "$bin" merge "${decks[@]}" "$@" -o "$out" --json > "$report"
    rc=$?
    end=$(date +%s.%N 2>/dev/null || date +%s)
    if [[ $rc -ge 128 ]]; then
        fail "$name: merge crashed (exit $rc)"
        return 1
    elif [[ $rc -ne 0 ]]; then
        fail "$name: merge exited $rc"
        return 1
    fi
    printf "\n== %s (%s): %s slides, %s KiB, %.2f s\n" "$name" "${*:-keep source}" \
        "$(jq '.slides' "$report")" "$(( $(jq '.output_bytes' "$report") / 1024 ))" \
        "$(echo "$end - $start" | bc)"
    jq -r '.inputs[] | "  \(.path | split("/") | last): \(.slides) slides, \(.parts_added) parts, " +
        "media dedup \(.media_deduplicated.count) (\(.media_deduplicated.bytes) B), " +
        "masters \(.masters.imported) imported / \(.masters.reused) reused" +
        (if .base then " [base]" else "" end) +
        (if (.warnings | length) > 0 then "\n    " + (.warnings | join("\n    ")) else "" end)' "$report"
    if "$bin" lint --strict --only integrity "$out" > "$work/$name.integrity.txt"; then
        echo "  integrity: clean"
    else
        fail "$name: integrity findings"
        sed 's/^/    /' "$work/$name.integrity.txt"
    fi
}

# same_slides NAME STRICT: diff each input against the output; every input
# slide must pair with its slot. STRICT=1 also requires "unchanged".
same_slides() {
    local name="$1" strict="$2" out="$work/$1.pptx"
    local offset=0 changed_total=0
    for deck in "${decks[@]}"; do
        local n diff_json
        n=$(slides_of "$deck")
        diff_json="$work/$name.$(basename "$deck" .pptx).diff.json"
        "$bin" diff --format json "$deck" "$out" > "$diff_json"
        if [[ $? -ge 2 ]]; then
            fail "$name: diff failed for $deck"
            continue
        fi
        # Slots this deck may pair with: its own, and those of any repeat.
        local allowed="[" o=0
        for other in "${decks[@]}"; do
            [[ "$other" == "$deck" ]] && allowed+="$o,"
            o=$(( o + $(slides_of "$other") ))
        done
        allowed="${allowed%,}]"
        local misplaced changed
        misplaced=$(jq --argjson offs "$allowed" '[.slides[] | select(.a_index != null) |
            select(.a_index as $a | .b_index as $b | ($offs | map(. + $a) | index($b)) == null)] | length' "$diff_json")
        changed=$(jq '[.slides[] | select(.a_index != null and .status != "unchanged")] | length' "$diff_json")
        local paired
        paired=$(jq '[.slides[] | select(.a_index != null and .b_index != null)] | length' "$diff_json")
        [[ "$paired" -ne "$n" ]] && fail "$name: $(basename "$deck"): $paired of $n slides paired"
        [[ "$misplaced" -ne 0 ]] && fail "$name: $(basename "$deck"): $misplaced slides out of place"
        if [[ "$changed" -ne 0 ]]; then
            if [[ "$strict" -eq 1 ]]; then
                fail "$name: $(basename "$deck"): $changed slides changed"
            fi
            changed_total=$((changed_total + changed))
        fi
        offset=$((offset + n))
    done
    if [[ "$strict" -eq 1 ]]; then
        echo "  same slides: every input slide pairs with its slot, unchanged"
    else
        echo "  same slides: every input slide pairs with its slot; $changed_total restyled slides differ"
    fi
}

echo "inputs:"
for deck in "${decks[@]}"; do
    printf "  %-40s %3s slides\n" "$(basename "$deck")" "$(slides_of "$deck")"
done

merge_and_check keep_source && same_slides keep_source 1
merge_and_check theme1 --theme=1 && same_slides theme1 0

echo
if [[ $failures -eq 0 ]]; then
    echo "all checks passed"
else
    echo "$failures check(s) failed"
    exit 1
fi
