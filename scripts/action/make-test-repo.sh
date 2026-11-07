#!/usr/bin/env bash
# Build the scratch repository the Action's self-test reviews
# (.github/workflows/action-test.yml):
#
#   scripts/action/make-test-repo.sh <dir>
#
# Two commits of fixture decks. From the first to the second, decks/a.pptx is
# edited (diff_base -> diff_text_edit), decks/gone.pptx is deleted,
# "decks/old name.pptx" is renamed unchanged, and decks/Bad.PPTX
# (integrity_problems) and clean.pptx (lint_clean) are added; notes.txt
# changes too and must be ignored. Expected: 5 decks changed, 7 lint errors,
# 5 lint warnings.

set -euo pipefail

fixtures=$(cd "$(dirname "$0")/../../tests/fixtures" && pwd)
mkdir -p "$1"
cd "$1"
git init -q
git config user.email test@example.com
git config user.name test
git config core.autocrlf false

mkdir decks
cp "$fixtures/diff_base.pptx" decks/a.pptx
cp "$fixtures/minimal.pptx" decks/gone.pptx
cp "$fixtures/tables_deck.pptx" "decks/old name.pptx"
echo one > notes.txt
git add -A
git commit -qm base

git rm -q decks/gone.pptx
git mv "decks/old name.pptx" decks/new-name.pptx
cp "$fixtures/diff_text_edit.pptx" decks/a.pptx
cp "$fixtures/integrity_problems.pptx" decks/Bad.PPTX
cp "$fixtures/lint_clean.pptx" clean.pptx
echo two >> notes.txt
git add -A
git commit -qm head
