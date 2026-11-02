#!/usr/bin/env bash
# The pptctl GitHub Action (action.yml): finds the .pptx files a change
# touches, lints them and diffs them against the base, into the job summary.
#
#   scripts/action/review.sh detect   # list the changed decks, output `count`
#   scripts/action/review.sh review   # lint and diff them; output `errors`,
#                                     # `warnings`; write the summary
#
# Configured through the environment, which action.yml fills from its inputs:
#
#   PPTCTL_WORK        scratch directory shared by the two phases (required)
#   PPTCTL_BASE        base commit; empty or all zeros means there is none, and
#                      every deck under PPTCTL_PATHS is linted, none diffed
#   PPTCTL_HEAD        head commit (default HEAD)
#   PPTCTL_PATHS       git pathspecs, one per line (default: the whole repo)
#   PPTCTL_LINT_ARGS   extra arguments for `pptctl lint`, word-split
#   PPTCTL_DIFF_ARGS   extra arguments for `pptctl diff`, word-split
#   PPTCTL_HTML_DIR    when set, `pptctl diff --format html` pages go here
#
# Outputs go to $GITHUB_OUTPUT and the report to $GITHUB_STEP_SUMMARY; both
# default to stdout, for running it by hand. Annotations go to stdout.

# shellcheck disable=SC2016  # the backticks in printf formats are Markdown
set -euo pipefail

work=${PPTCTL_WORK:?PPTCTL_WORK is not set}
list=$work/decks.tsv # status<TAB>old path<TAB>new path, one deck per line
outputs=${GITHUB_OUTPUT:-/dev/stdout}
summary=${GITHUB_STEP_SUMMARY:-/dev/stdout}
head_rev=${PPTCTL_HEAD:-HEAD}
base_rev=${PPTCTL_BASE:-}
[[ $base_rev =~ ^0*$ ]] && base_rev=

# jq on Windows ends its lines with CRLF, which breaks $(( )) on its counts.
jq() { command jq "$@" | tr -d '\r'; }

# Longest diff shown per deck; the job summary is capped at 1 MiB per step.
max_diff_lines=400

# git diff --name-status -z: "M\0path\0", or "R100\0old\0new\0" for a rename.
parse_name_status() {
    local status old new
    while IFS= read -r -d '' status; do
        IFS= read -r -d '' old
        case $status in
            R* | C*) IFS= read -r -d '' new ;;
            *) new=$old ;;
        esac
        printf '%s\t%s\t%s\n' "${status:0:1}" "$old" "$new"
    done
}

detect() {
    local specs=() line
    while IFS= read -r line; do
        [ -n "$line" ] && specs+=("$line")
    done <<< "${PPTCTL_PATHS:-}"
    [ ${#specs[@]} -eq 0 ] && specs=(.)

    mkdir -p "$work"
    if [ -n "$base_rev" ]; then
        # actions/checkout fetches one commit; the base is usually not in it.
        git cat-file -e "$base_rev^{commit}" 2> /dev/null ||
            git fetch --quiet --no-tags --depth=1 origin "$base_rev"
        git diff --name-status -z -M "$base_rev" "$head_rev" -- "${specs[@]}" | parse_name_status
    else
        git ls-tree -r -z --name-only "$head_rev" -- "${specs[@]}" |
            while IFS= read -r -d '' line; do printf 'L\t%s\t%s\n' "$line" "$line"; done
    fi | {
        shopt -s nocasematch
        while IFS=$'\t' read -r status old new; do
            [[ $new == *.pptx ]] && printf '%s\t%s\t%s\n' "$status" "$old" "$new"
        done
        true
    } > "$list"

    local count
    count=$(wc -l < "$list" | tr -d ' ')
    echo "count=$count" >> "$outputs"
    echo "pptctl: $count deck(s) changed"
}

# Write the deck at <rev>:<path> to <dest>. A Git LFS pointer goes through
# `git lfs smudge`, which fetches the real file; returns 1 if that fails.
materialize() {
    local rev=$1 path=$2 dest=$3
    git show "$rev:$path" > "$dest"
    if head -c 64 "$dest" | grep -q '^version https://git-lfs'; then
        git lfs smudge -- "$path" < "$dest" > "$dest.lfs" 2> /dev/null || return 1
        mv "$dest.lfs" "$dest"
    fi
}

# Workflow-command escaping: %, CR, LF everywhere; also : and , in properties.
annotation() {
    local level=$1 file=$2 title=$3 message=$4
    jq -rn --arg level "$level" --arg file "$file" --arg title "$title" --arg msg "$message" '
        def data: gsub("%"; "%25") | gsub("\r"; "%0D") | gsub("\n"; "%0A");
        def prop: data | gsub(":"; "%3A") | gsub(","; "%2C");
        "::\($level) file=\($file | prop),title=\($title | prop)::\($msg | data)"'
}

plural() { # count noun
    if [ "$1" -eq 1 ]; then echo "1 $2"; else echo "$1 $2s"; fi
}

# Lint <deck file> as <repo path>: annotations to stdout, findings table to
# $details, counts added to $errors / $warnings, and the overview cell in
# $lint_cell.
lint_deck() {
    local file=$1 path=$2 dir=$3 rc=0
    # shellcheck disable=SC2086  # word-splitting the extra arguments is intended
    pptctl lint --json ${PPTCTL_LINT_ARGS:-} "$file" > "$dir/lint.json" 2> "$dir/lint.err" || rc=$?

    local e=0 w=0
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 2 ]; then
        # Exit 1: not a ZIP (no report), or presentation.xml unreadable (the
        # integrity report is still written). Either way the deck is broken.
        local why
        why=$(tr '\n' ' ' < "$dir/lint.err")
        why=${why//"$file"/$path}
        annotation error "$path" "pptctl lint" "could not be read as a .pptx: ${why:-exit $rc}"
        printf -- '- **error**: could not be read as a .pptx: %s\n\n' "${why:-exit $rc}" >> "$details"
        e=1
    fi
    if [ -s "$dir/lint.json" ]; then
        jq -r --arg file "$path" '
            def data: gsub("%"; "%25") | gsub("\r"; "%0D") | gsub("\n"; "%0A");
            def prop: data | gsub(":"; "%3A") | gsub(","; "%2C");
            def where: [(.slide // empty | "slide \(.)"), (.shape // empty | "shape \"\(.)\""),
                        (.part // empty)] | join(", ");
            .diagnostics[]
            | "::\(.severity) file=\($file | prop),title=\("pptctl lint: \(.rule)" | prop)::"
              + ((if where == "" then "" else where + ": " end) + .message | data)
        ' "$dir/lint.json"
        e=$((e + $(jq '.summary.errors' "$dir/lint.json")))
        w=$((w + $(jq '.summary.warnings' "$dir/lint.json")))
        jq -r '
            def cell: gsub("\\|"; "\\|") | gsub("[\r\n]+"; " ");
            def where: [(.slide // empty | "slide \(.)"), (.shape // empty | "shape \"\(.)\""),
                        (.part // empty | "`\(.)`")] | join(", ");
            if (.diagnostics | length) > 0 then
                "| Severity | Rule | Where | Message |", "|---|---|---|---|",
                (.diagnostics[] | "| \(.severity) | `\(.rule)` | \(where | if . == "" then "deck" else cell end) | \(.message | cell) |"),
                ""
            else empty end
        ' "$dir/lint.json" >> "$details"
    fi

    errors=$((errors + e))
    warnings=$((warnings + w))
    if [ $((e + w)) -eq 0 ]; then
        lint_cell="clean"
        printf 'No lint findings.\n\n' >> "$details"
    elif [ "$e" -eq 0 ]; then
        lint_cell=$(plural "$w" warning)
    elif [ "$w" -eq 0 ]; then
        lint_cell=$(plural "$e" error)
    else
        lint_cell="$(plural "$e" error), $(plural "$w" warning)"
    fi
}

# Diff <old file> against <new file>, both shown under their repo paths.
diff_deck() {
    local oldf=$1 newf=$2 old=$3 new=$4 dir=$5 rc=0
    # shellcheck disable=SC2086  # word-splitting the extra arguments is intended
    pptctl diff ${PPTCTL_DIFF_ARGS:-} "$oldf" "$newf" > "$dir/diff.txt" 2> "$dir/diff.err" || rc=$?
    case $rc in
        0)
            echo "No changes to slide content." >> "$details"
            echo >> "$details"
            return
            ;;
        1) ;;
        *)
            printf 'Could not diff: %s\n\n' "$(tr '\n' ' ' < "$dir/diff.err")" >> "$details"
            return
            ;;
    esac

    # Unified output names the temporary files; show the repo paths instead.
    # Other formats (--format json) pass through.
    if head -n 1 "$dir/diff.txt" | grep -q '^--- '; then
        { printf -- '--- a/%s\n+++ b/%s\n' "$old" "$new"; tail -n +3 "$dir/diff.txt"; } > "$dir/diff.named"
        mv "$dir/diff.named" "$dir/diff.txt"
    fi
    local lines lang=diff
    lines=$(wc -l < "$dir/diff.txt" | tr -d ' ')
    head -c 1 "$dir/diff.txt" | grep -q '[{[]' && lang=json
    {
        echo "<details open><summary>Slide diff</summary>"
        echo
        echo "\`\`\`\`$lang"
        head -n "$max_diff_lines" "$dir/diff.txt"
        echo "\`\`\`\`"
        if [ "$lines" -gt "$max_diff_lines" ]; then
            echo
            echo "_Showing $max_diff_lines of $lines lines._"
        fi
        echo
        echo "</details>"
        echo
    } >> "$details"

    if [ -n "${PPTCTL_HTML_DIR:-}" ]; then
        mkdir -p "$PPTCTL_HTML_DIR"
        pptctl diff --format html "$oldf" "$newf" > "$PPTCTL_HTML_DIR/${new//\//__}.html" || true
    fi
}

review() {
    local overview=$work/overview.md details=$work/details.md
    : > "$overview"
    : > "$details"
    errors=0
    warnings=0

    local n=0 status old new dir change lint_cell
    # The list is read on fd 3: git, jq and pptctl all inherit stdin.
    while IFS=$'\t' read -r -u 3 status old new; do
        n=$((n + 1))
        dir=$work/$n
        mkdir -p "$dir"
        lint_cell="—"
        case $status in
            A) change="added" ;;
            D) change="deleted" ;;
            R) change="renamed from \`$old\`" ;;
            L) change="—" ;;
            *) change="modified" ;;
        esac
        # A deleted deck has nothing to lint or diff: its overview row is all.
        case $status in
            D) ;;
            L) printf '#### `%s`\n\n' "$new" >> "$details" ;;
            *) printf '#### `%s` (%s)\n\n' "$new" "$change" >> "$details" ;;
        esac

        if [ "$status" != D ]; then
            if ! materialize "$head_rev" "$new" "$dir/new.pptx"; then
                annotation error "$new" "pptctl" "is a Git LFS pointer and the file could not be fetched"
                printf -- '- **error**: Git LFS pointer; the file could not be fetched\n\n' >> "$details"
                errors=$((errors + 1))
                printf '| `%s` | %s | could not fetch |\n' "$new" "$change" >> "$overview"
                continue
            fi
            if [ "$status" = M ] || [ "$status" = R ] || [ "$status" = T ]; then
                if materialize "$base_rev" "$old" "$dir/old.pptx"; then
                    diff_deck "$dir/old.pptx" "$dir/new.pptx" "$old" "$new" "$dir"
                else
                    printf 'Could not diff: the base version is a Git LFS pointer that could not be fetched.\n\n' >> "$details"
                fi
            fi
            lint_deck "$dir/new.pptx" "$new" "$dir"
        fi
        printf '| `%s` | %s | %s |\n' "$new" "$change" "$lint_cell" >> "$overview"
    done 3< "$list"

    {
        echo "errors=$errors"
        echo "warnings=$warnings"
    } >> "$outputs"
    {
        if [ -n "$base_rev" ]; then
            echo "### pptctl: $(plural "$n" deck) changed"
        else
            echo "### pptctl: $(plural "$n" deck)"
        fi
        echo
        echo "| Deck | Change | Lint |"
        echo "|---|---|---|"
        cat "$overview"
        echo
        cat "$details"
    } >> "$summary"
    echo "pptctl: $(plural "$errors" error), $(plural "$warnings" warning) in $(plural "$n" deck)"
}

case ${1:-} in
    detect) detect ;;
    review) review ;;
    *)
        echo "usage: $0 detect|review" >&2
        exit 64
        ;;
esac
