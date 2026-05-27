#!/usr/bin/env bash
# Validate `pptctl info` against a directory of real-world .pptx decks.
#
# Runs `info` (text + --json) on every .pptx in a directory and classifies each
# result as OK, a graceful error (clean non-zero exit), or a CRASH (killed by a
# signal — segfault, abort, ASan trap, etc.). Exits non-zero if any deck crashes.
#
# Usage:
#   scripts/validate_info.sh [DECK_DIR] [--bin PATH] [--logs DIR]
#
#   DECK_DIR    directory of .pptx files (default: tests/real_world)
#   --bin PATH  pptctl binary to test (default: build/macos-release/pptctl)
#   --logs DIR  where to write per-deck output logs (default: DECK_DIR/.logs)
#
# A deck dir + logs are intentionally kept out of git (see .gitignore).

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
    logs="$deck_dir/.logs"
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

# Classify one invocation. Echoes "OK", "ERROR", or "CRASH" and returns the
# exit code of the run. Signal deaths (exit >= 128) are crashes.
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

ok=0
err=0
crash=0
total=0
crashed_files=()

printf '%-45s %-7s %-7s\n' "deck" "text" "json"
printf '%-45s %-7s %-7s\n' "----" "----" "----"

# NUL-delimited to survive spaces in filenames.
while IFS= read -r -d '' f; do
    total=$((total + 1))
    base="$(basename "$f")"
    safe="${base//\//_}"

    "$bin" info "$f" >"$logs/$safe.text.out" 2>"$logs/$safe.text.err"
    text_code=$?
    text_class="$(classify "$text_code")"

    "$bin" info --json "$f" >"$logs/$safe.json.out" 2>"$logs/$safe.json.err"
    json_code=$?
    json_class="$(classify "$json_code")"

    # Worst outcome across both modes decides the file's fate.
    if [[ "$text_class" == "CRASH" || "$json_class" == "CRASH" ]]; then
        crash=$((crash + 1))
        crashed_files+=("$base (text=$text_code json=$json_code)")
    elif [[ "$text_class" == "ERROR" || "$json_class" == "ERROR" ]]; then
        err=$((err + 1))
    else
        ok=$((ok + 1))
    fi

    name="$base"
    if [[ ${#name} -gt 44 ]]; then
        name="${name:0:41}..."
    fi
    printf '%-45s %-7s %-7s\n' "$name" "$text_class" "$json_class"
done < <(find "$deck_dir" -type f \( -iname '*.pptx' -o -iname '*.ppsx' -o -iname '*.potx' \) -print0 | sort -z)

echo
echo "binary:  $bin"
echo "decks:   $total   ok: $ok   errors: $err   crashes: $crash"
echo "logs:    $logs"

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
exit 0
