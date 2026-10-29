#!/usr/bin/env bash
# Validate `pptctl optimize` against a directory of real-world .pptx decks (#107).
#
# For every deck:
#   1. optimize it (all passes, defaults) with its --json report, timing and
#      peak RSS                       -> size before/after, % saved, per pass
#   2. diff deck vs output (JSON)     -> every slide pairs, nothing moves, and
#                                        the only ops are picture "replaced"
#   3. extract deck and output (JSON) -> identical once the input path and
#                                        picture sizes are set aside
#   4. optimize the output again      -> saves nothing (idempotent)
# then prints the mean and median reduction.
#
# Usage:
#   scripts/validate_optimize.sh [DECK_DIR] [--bin PATH] [--work DIR]
#
#   DECK_DIR    directory of .pptx files (default: tests/real_world)
#   --bin PATH  pptctl binary to test (default: build/macos-release/pptctl)
#   --work DIR  where outputs, reports and logs go
#               (default: DECK_DIR/.work-optimize, gitignored)
#
# Needs jq. Timing and RSS come from macOS's /usr/bin/time -l.

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
[[ -z "$work" ]] && work="$deck_dir/.work-optimize"

if [[ ! -x "$bin" ]]; then
    echo "error: binary not found: $bin (build first with ./scripts/build.sh)" >&2
    exit 2
fi
if [[ ! -d "$deck_dir" ]]; then
    echo "error: deck directory not found: $deck_dir" >&2
    exit 2
fi
if ! command -v jq >/dev/null; then
    echo "error: validate_optimize.sh needs jq" >&2
    exit 2
fi
mkdir -p "$work"

classify() {
    local code="$1"
    if [[ "$code" -ge 128 ]]; then echo "CRASH"; else echo "$code"; fi
}

# Bytes saved per pass, as "id:KiB" words for the passes that saved anything.
pass_breakdown() {
    jq -r '[.passes[]
            | {id, saved: ([.actions[] | .bytes_before - .bytes_after] | add // 0)}
            | select(.saved > 0)
            | "\(.id):\((.saved / 1024) | floor)K"] | join(" ")' "$1"
}

# Picture sizes and the input path are expected to change; nothing else is.
extract_canon='del(.file) | walk(if type == "object" and has("size_bytes") then del(.size_bytes) else . end)'

# Diff verdict: "ok" when every slide pairs, unmoved, with only replaced pictures.
diff_verdict='
    ([.slides[] | select(.status == "added" or .status == "removed" or .moved)] | length) as $unpaired
    | ([.slides[] | .changes // {} | (.text + .shapes + .tables + .charts) | length] | add // 0) as $other
    | ([.slides[] | .changes // {} | .pictures[] | select(.op != "replaced")] | length) as $pics
    | if $unpaired > 0 then "UNPAIRED"
      elif $other > 0 then "NONPIC"
      elif $pics > 0 then "PICOP"
      else "ok" end'

total=0
crashes=0
semantic_ok=0
stable_ok=0
reductions=()
printf '%-34s %9s %9s %6s %6s %7s %-5s %-5s %-6s %s\n' \
    "deck" "before" "after" "saved" "time" "rss" "diff" "text" "again" "per pass"
printf '%-34s %9s %9s %6s %6s %7s %-5s %-5s %-6s %s\n' \
    "----" "------" "-----" "-----" "----" "---" "----" "----" "-----" "--------"

while IFS= read -r -d '' f; do
    total=$((total + 1))
    name="$(basename "${f%.pptx}")"
    out="$work/$name.opt.pptx"
    report="$work/$name.report.json"

    /usr/bin/time -l "$bin" optimize --json -o "$out" "$f" >"$report" 2>"$work/$name.time"
    rc="$(classify $?)"
    if [[ "$rc" != "0" ]]; then
        [[ "$rc" == "CRASH" ]] && crashes=$((crashes + 1))
        printf '%-34s optimize failed: %s\n' "${name:0:33}" "$rc"
        continue
    fi
    secs="$(awk '/ real /{print $1}' "$work/$name.time")"
    rss_mb="$(awk '/maximum resident set size/{printf "%.0fM", $1 / 1048576}' "$work/$name.time")"
    before="$(jq .input_bytes "$report")"
    after="$(jq .output_bytes "$report")"
    pct="$(awk -v b="$before" -v a="$after" 'BEGIN{printf "%.1f", (b - a) * 100 / b}')"
    reductions+=("$pct")

    "$bin" diff --format json "$f" "$out" >"$work/$name.diff.json" 2>"$work/$name.diff.err"
    drc="$(classify $?)"
    [[ "$drc" == "CRASH" ]] && crashes=$((crashes + 1))
    if [[ "$drc" == "0" || "$drc" == "1" ]]; then
        dv="$(jq -r "$diff_verdict" "$work/$name.diff.json")"
    else
        dv="rc=$drc"
    fi

    "$bin" extract --format json "$f" | jq -S "$extract_canon" >"$work/$name.in.extract.json"
    "$bin" extract --format json "$out" | jq -S "$extract_canon" >"$work/$name.out.extract.json"
    if cmp -s "$work/$name.in.extract.json" "$work/$name.out.extract.json"; then
        tv="ok"
    else
        tv="DIFF"
    fi
    [[ "$dv" == "ok" && "$tv" == "ok" ]] && semantic_ok=$((semantic_ok + 1))

    "$bin" optimize --json --dry-run "$out" >"$work/$name.again.json" 2>&1
    arc="$(classify $?)"
    [[ "$arc" == "CRASH" ]] && crashes=$((crashes + 1))
    if [[ "$arc" == "0" ]] &&
        jq -e '.output_bytes == .input_bytes and ([.passes[].actions[] | select(.kind != "skipped")] | length == 0)' \
            "$work/$name.again.json" >/dev/null; then
        av="ok"
        stable_ok=$((stable_ok + 1))
    else
        av="CHANGES"
    fi

    printf '%-34s %8sK %8sK %5s%% %5ss %7s %-5s %-5s %-6s %s\n' \
        "${name:0:33}" "$((before / 1024))" "$((after / 1024))" "$pct" "$secs" "$rss_mb" \
        "$dv" "$tv" "$av" "$(pass_breakdown "$report")"
done < <(find "$deck_dir" -maxdepth 1 -type f -iname '*.pptx' -print0 | sort -z)

echo
echo "binary:   $bin"
echo "decks:    $total   crashes: $crashes"
if [[ ${#reductions[@]} -gt 0 ]]; then
    printf '%s\n' "${reductions[@]}" | sort -n | awk '
        { v[NR] = $1; s += $1 }
        END {
            m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
            printf "saved:    mean %.1f%%, median %.1f%%, min %.1f%%, max %.1f%%\n", s / NR, m, v[1], v[NR]
        }'
fi
echo "same:     $semantic_ok/$total decks with only replaced pictures and identical extract"
echo "stable:   $stable_ok/$total outputs that a second optimize leaves alone"
echo "work:     $work"

[[ "$crashes" -gt 0 ]] && exit 1
[[ "$semantic_ok" -lt "$total" || "$stable_ok" -lt "$total" ]] && exit 1
exit 0
