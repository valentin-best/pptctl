#!/usr/bin/env bash
# Package a release binary with the files that ship beside it:
#
#   scripts/release/package.sh <binary> <target> <out-dir>
#
# Writes <out-dir>/pptctl-<version>-<target>.tar.gz, or .zip for a Windows
# .exe, holding one pptctl-<version>-<target>/ directory. The version is the
# binary's own `--version`, so an archive can't be named after the wrong one.

set -euo pipefail

bin=$1
target=$2
mkdir -p "$3"
out=$(cd "$3" && pwd)

version=$("$bin" --version)
name="pptctl-$version-$target"
stage=$(mktemp -d)
mkdir "$stage/$name"
cp "$bin" scripts/git-pptx-diff.sh scripts/git-pptx-difftool.sh LICENSE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md "$stage/$name/"
cp -R skills "$stage/$name/"

cd "$stage"
case $bin in
    *.exe) 7z a -tzip -bso0 "$out/$name.zip" "$name" ;;
    *) tar -czf "$out/$name.tar.gz" "$name" ;;
esac
rm -rf "$stage"
ls -l "$out"
