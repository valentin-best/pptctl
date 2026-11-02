#!/bin/sh
# git difftool for .pptx files: writes `pptctl diff --format html` of the two
# versions to a file and opens it in the browser. See docs/git-integration.md.
#
# Install:
#   git config difftool.pptx.cmd 'git-pptx-difftool.sh "$LOCAL" "$REMOTE" "$MERGED"'
# Use:
#   git difftool -y -t pptx HEAD~1 -- '*.pptx'
#
# The page is self-contained and kept in ${TMPDIR:-/tmp}/pptctl-difftool/, so it
# outlives the temporary copies git deletes when this script returns. Extra
# options for pptctl come from $PPTCTL_DIFF_ARGS. $PPTCTL_OPEN names the command
# that opens the page (default: open on macOS, xdg-open on Linux, start on
# Windows); PPTCTL_OPEN=none only prints its path.

local_file=$1
remote_file=$2
path=${3:-$2}

# An added or deleted file is /dev/null, or nul with Git for Windows.
if [ "$local_file" = /dev/null ] || [ "$local_file" = nul ]; then
    printf 'pptctl diff: new file %s\n' "$path"
    exit 0
fi
if [ "$remote_file" = /dev/null ] || [ "$remote_file" = nul ]; then
    printf 'pptctl diff: deleted file %s\n' "$path"
    exit 0
fi

dir=${TMPDIR:-/tmp}
dir=${dir%/}/pptctl-difftool
mkdir -p "$dir" || exit 1
name=$(basename "$path" .pptx)
out=$(mktemp "$dir/$name.XXXXXX") || exit 1
mv "$out" "$out.html" && out=$out.html

# The page names the two files it compares; show the repo path, not git's
# temporary copies. Escaped for HTML, then for the sed replacement.
label() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' \
        -e 's/[\\|&]/\\&/g'
}
pattern() { # a literal path as a basic regex
    printf '%s' "$1" | sed -e 's/[]\\|.*^$[]/\\&/g'
}

# shellcheck disable=SC2086  # word-splitting $PPTCTL_DIFF_ARGS is intended
pptctl diff --format html $PPTCTL_DIFF_ARGS "$local_file" "$remote_file" > "$out"
rc=$?
if [ "$rc" -gt 1 ]; then
    rm -f "$out"
    exit "$rc"
fi
sed -e "s|<code class=\"path\">$(pattern "$local_file")</code>|<code class=\"path\">a/$(label "$path")</code>|" \
    -e "s|<code class=\"path\">$(pattern "$remote_file")</code>|<code class=\"path\">b/$(label "$path")</code>|" \
    "$out" > "$out.tmp" && mv "$out.tmp" "$out"

printf '%s: %s\n' "$path" "$out"
opener=${PPTCTL_OPEN:-}
if [ -z "$opener" ]; then
    case $(uname -s) in
        Darwin) opener=open ;;
        MINGW* | MSYS* | CYGWIN*) opener=start ;;
        *) opener=xdg-open ;;
    esac
fi
# shellcheck disable=SC2086  # PPTCTL_OPEN may carry arguments
case $opener in
    none) ;;
    start) cmd //c start "" "$(cygpath -w "$out")" ;;
    *) $opener "$out" > /dev/null 2>&1 || echo "could not run '$opener'; open the file above" >&2 ;;
esac
exit 0
