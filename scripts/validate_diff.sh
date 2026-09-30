#!/usr/bin/env bash
# Validate `pptctl diff` against a directory of real-world .pptx decks (#77).
#
# For every deck:
#   1. self-diff            -> must be identical (exit 0, no output)
#   2. deck vs re-saved     -> python-pptx round-trip with no edits; measures
#                              the re-save noise floor (ideally identical)
#   3. deck vs edited       -> three known edits (text, shape move, last slide
#                              removed); scored by validate_diff_edits.py
# and records wall time and peak RSS for the largest pair.
#
# Usage:
#   scripts/validate_diff.sh [DECK_DIR] [--bin PATH] [--work DIR]
#
#   DECK_DIR    directory of .pptx files (default: tests/real_world)
#   --bin PATH  pptctl binary to test (default: build/macos-release/pptctl)
#   --work DIR  where edited decks, JSON reports and logs go
#               (default: DECK_DIR/.work-diff, gitignored)
#
# Needs `uv` for the python-pptx edit/score helper (dev-only, see CLAUDE.md).

set -u

deck_dir="tests/real_world"
bin="build/macos-release/pptctl"
work=""
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin) bin="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
[[ ${#args[@]} -gt 0 ]] && deck_dir="${args[0]}"
[[ -z "$work" ]] && work="$deck_dir/.work-diff"

if [[ ! -x "$bin" ]]; then
    echo "error: binary not found: $bin (build first with ./scripts/build.sh)" >&2
    exit 2
fi
if [[ ! -d "$deck_dir" ]]; then
    echo "error: deck directory not found: $deck_dir" >&2
    exit 2
fi
mkdir -p "$work"
helper="$(dirname "$0")/validate_diff_edits.py"

classify() {
    local code="$1"
    if [[ "$code" -ge 128 ]]; then echo "CRASH"; else echo "$code"; fi
}

total=0
self_ok=0
resave_ok=0
edits_ok=0
crashes=0
printf '%-40s %-6s %-8s %-8s\n' "deck" "self" "resaved" "edited"
printf '%-40s %-6s %-8s %-8s\n' "----" "----" "-------" "------"

while IFS= read -r -d '' f; do
    total=$((total + 1))
    base="$(basename "$f")"
    name="${base%.pptx}"

    "$bin" diff -q "$f" "$f" 2>"$work/$name.self.err"
    self_rc="$(classify $?)"
    [[ "$self_rc" == "0" ]] && self_ok=$((self_ok + 1))
    [[ "$self_rc" == "CRASH" ]] && crashes=$((crashes + 1))

    if ! uv run --python 3.12 --with python-pptx "$helper" edit "$f" "$work" >"$work/$name.edit.log" 2>&1; then
        printf '%-40s %-6s %-8s %-8s\n' "${name:0:39}" "$self_rc" "EDITFAIL" "-"
        continue
    fi

    "$bin" diff --no-color "$f" "$work/$name.resaved.pptx" >"$work/$name.resaved.txt" 2>&1
    resave_rc="$(classify $?)"
    [[ "$resave_rc" == "0" ]] && resave_ok=$((resave_ok + 1))
    [[ "$resave_rc" == "CRASH" ]] && crashes=$((crashes + 1))

    "$bin" diff --format json "$f" "$work/$name.edited.pptx" >"$work/$name.edited.json" 2>"$work/$name.edited.err"
    edit_rc="$(classify $?)"
    "$bin" diff --no-color "$f" "$work/$name.edited.pptx" >"$work/$name.edited.txt" 2>&1
    [[ "$edit_rc" == "CRASH" ]] && crashes=$((crashes + 1))
    if [[ "$edit_rc" == "1" ]]; then
        if uv run --python 3.12 --with python-pptx "$helper" score "$work/$name.expected.json" "$work/$name.edited.json" >"$work/$name.score.txt" 2>&1; then
            edits_ok=$((edits_ok + 1))
            edit_class="found"
        else
            edit_class="MISSING"
        fi
    else
        edit_class="rc=$edit_rc"
    fi
    printf '%-40s %-6s %-8s %-8s\n' "${name:0:39}" "$self_rc" "$resave_rc" "$edit_class"
done < <(find "$deck_dir" -maxdepth 1 -type f -iname '*.pptx' -print0 | sort -z)

echo
echo "binary:   $bin"
echo "decks:    $total   crashes: $crashes"
echo "self:     $self_ok/$total identical"
echo "resaved:  $resave_ok/$total identical after a python-pptx round-trip (noise floor)"
echo "edits:    $edits_ok/$total decks where every known edit was reported"
echo
echo "scorecards:"
cat "$work"/*.score.txt 2>/dev/null
echo

# Largest pair: time and memory.
largest="$(find "$deck_dir" -maxdepth 1 -type f -iname '*.pptx' -print0 | xargs -0 ls -S | head -1)"
lname="$(basename "${largest%.pptx}")"
if [[ -f "$work/$lname.edited.pptx" ]]; then
    echo "largest pair: $lname ($(du -h "$largest" | cut -f1))"
    /usr/bin/time -l "$bin" diff -q "$largest" "$work/$lname.edited.pptx" 2>&1 | grep -E "real|maximum resident" | sed 's/^/  /'
fi
echo "work:     $work"

[[ "$crashes" -gt 0 ]] && exit 1
[[ "$edits_ok" -lt "$total" ]] && exit 1
exit 0
