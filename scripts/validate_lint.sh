#!/usr/bin/env bash
# Validate `pptctl lint` against a directory of real-world .pptx decks.
#
# Runs `lint` (text + --json) on every .pptx in a directory, classifies each
# result as OK, a graceful error (clean non-zero exit), or a CRASH (killed by a
# signal — segfault, abort, ASan trap, etc.), and tallies findings per rule
# from the JSON output. Prints a per-deck table and the Phase 3 exit-criterion
# ratio (decks with at least one finding). Exits non-zero if any deck crashes
# or if fewer than half the decks surface a finding.
#
# Usage:
#   scripts/validate_lint.sh [DECK_DIR] [--bin PATH] [--logs DIR]
#
#   DECK_DIR    directory of .pptx files (default: tests/real_world)
#   --bin PATH  pptctl binary to test (default: build/macos-release/pptctl)
#   --logs DIR  where to write per-deck output logs (default: DECK_DIR/.logs-lint)
#
# Deck dir + logs are intentionally kept out of git (see .gitignore).

set -u

deck_dir="tests/real_world"
bin="build/macos-release/pptctl"
logs=""

args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin)
            bin="$2"
            shift 2
            ;;
        --logs)
            logs="$2"
            shift 2
            ;;
        *)
            args+=("$1")
            shift
            ;;
    esac
done
if [[ ${#args[@]} -gt 0 ]]; then
    deck_dir="${args[0]}"
fi
if [[ -z "$logs" ]]; then
    logs="$deck_dir/.logs-lint"
fi

if [[ ! -x "$bin" ]]; then
    echo "error: binary not found or not executable: $bin" >&2
    echo "       build it first with ./scripts/build.sh" >&2
    exit 2
fi
if [[ ! -d "$deck_dir" ]]; then
    echo "error: deck directory not found: $deck_dir" >&2
    exit 2
fi

mkdir -p "$logs"

# Keep in sync with the rule ids in include/pptctl/lint.hpp.
rules=(overflow tiny-text alt-text font-risk empty-ph ctype img-bloat)

classify() {
    local code="$1"
    if [[ "$code" -eq 0 ]]; then
        echo "OK"
    elif [[ "$code" -ge 128 ]]; then
        echo "CRASH"
    else
        echo "ERROR"
    fi
}

# Count diagnostics for one rule in a JSON log. grep-only so the script has
# no jq dependency; the "rule" field is on its own line in pptctl's output.
count_rule() {
    grep -c "\"rule\": \"$1\"" "$2" 2>/dev/null || true
}

ok=0
err=0
crash=0
total=0
with_findings=0
crashed_files=()
declare -a rule_totals
for _ in "${rules[@]}"; do rule_totals+=(0); done

printf '%-30s %-6s %-6s' "deck" "text" "json"
for r in "${rules[@]}"; do printf ' %9s' "$r"; done
printf ' %6s\n' "total"
printf '%-30s %-6s %-6s' "----" "----" "----"
for _ in "${rules[@]}"; do printf ' %9s' "---------"; done
printf ' %6s\n' "-----"

while IFS= read -r -d '' f; do
    total=$((total + 1))
    base="$(basename "$f")"
    safe="${base//\//_}"

    "$bin" lint "$f" >"$logs/$safe.text.out" 2>"$logs/$safe.text.err"
    text_class="$(classify $?)"
    "$bin" lint --json "$f" >"$logs/$safe.json.out" 2>"$logs/$safe.json.err"
    json_code=$?
    json_class="$(classify $json_code)"

    if [[ "$text_class" == "CRASH" || "$json_class" == "CRASH" ]]; then
        crash=$((crash + 1))
        crashed_files+=("$base")
    elif [[ "$text_class" == "ERROR" || "$json_class" == "ERROR" ]]; then
        err=$((err + 1))
    else
        ok=$((ok + 1))
    fi

    name="${base%.pptx}"
    if [[ ${#name} -gt 29 ]]; then
        name="${name:0:26}..."
    fi
    printf '%-30s %-6s %-6s' "$name" "$text_class" "$json_class"
    deck_total=0
    i=0
    for r in "${rules[@]}"; do
        n="$(count_rule "$r" "$logs/$safe.json.out")"
        n="${n:-0}"
        printf ' %9s' "$n"
        deck_total=$((deck_total + n))
        rule_totals[$i]=$((rule_totals[$i] + n))
        i=$((i + 1))
    done
    printf ' %6s\n' "$deck_total"
    if [[ "$json_code" -eq 0 && "$deck_total" -gt 0 ]]; then
        with_findings=$((with_findings + 1))
    fi
done < <(find "$deck_dir" -type f \( -iname '*.pptx' -o -iname '*.ppsx' -o -iname '*.potx' \) -print0 | sort -z)

printf '%-30s %-6s %-6s' "TOTAL" "" ""
grand=0
for n in "${rule_totals[@]}"; do
    printf ' %9s' "$n"
    grand=$((grand + n))
done
printf ' %6s\n' "$grand"

echo
echo "binary:   $bin"
echo "decks:    $total   ok: $ok   errors: $err   crashes: $crash"
if [[ "$total" -gt 0 ]]; then
    pct=$((with_findings * 100 / total))
    echo "findings: $with_findings/$total decks surface >= 1 diagnostic ($pct%; exit criterion: >= 50%)"
fi
echo "logs:     $logs"

if [[ "$crash" -gt 0 ]]; then
    echo
    echo "CRASHES:"
    for c in "${crashed_files[@]}"; do
        echo "  - $c"
    done
    exit 1
fi
if [[ "$total" -eq 0 ]]; then
    echo "warning: no decks found in $deck_dir" >&2
    exit 2
fi
if [[ $((with_findings * 2)) -lt "$total" ]]; then
    echo "exit criterion NOT met" >&2
    exit 1
fi
exit 0
