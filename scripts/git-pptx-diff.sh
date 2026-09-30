#!/bin/sh
# git external diff driver for .pptx files: runs `pptctl diff` on the two
# versions git hands us. See docs/git-integration.md.
#
# Install:
#   git config diff.pptx.command "$PWD/scripts/git-pptx-diff.sh"
#   echo '*.pptx diff=pptx' >> .gitattributes
#
# git calls the driver with seven arguments:
#   path old-file old-hex old-mode new-file new-hex new-mode
# old-file / new-file are temporary copies (or /dev/null for an added or
# deleted file). Extra options for pptctl come from $PPTCTL_DIFF_ARGS, e.g.
# PPTCTL_DIFF_ARGS="--format json".
#
# Always exits 0: git treats a non-zero exit from a diff driver as an error
# and aborts the whole diff, while pptctl exits 1 whenever the decks differ.

path=$1
old=$2
new=$5

if [ "$old" = /dev/null ]; then
    printf 'pptctl diff: new file %s\n' "$path"
    exit 0
fi
if [ "$new" = /dev/null ]; then
    printf 'pptctl diff: deleted file %s\n' "$path"
    exit 0
fi

# Unified output names the temporary files git hands us; show the repo path
# instead. The other formats do not match and pass through untouched.
# shellcheck disable=SC2086  # word-splitting $PPTCTL_DIFF_ARGS is intended
pptctl diff $PPTCTL_DIFF_ARGS "$old" "$new" |
    sed -e "s|^--- $old\$|--- a/$path|" -e "s|^+++ $new\$|+++ b/$path|"
exit 0
