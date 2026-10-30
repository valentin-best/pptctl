#!/usr/bin/env python3
# Which src/*.cpp files the CI clang-tidy job checks for a change.
#
#   tidy_files.py [BASE [HEAD]]     (default: HEAD^1 HEAD, a PR's merge commit)
#
# Prints the files, one per line, sorted; explains the choice on stderr.
#
#   - Every file, when the change touches something that can affect any of
#     them: .clang-tidy, CMakePresets.json, cmake/, the CI workflow (the
#     clang-tidy version), this script, or a CMakeLists.txt line other than a
#     source/test list entry (flags, dependencies, targets).
#   - Otherwise the changed src/*.cpp files that still exist, plus every
#     src/*.cpp that includes a changed header, directly or through other
#     headers. Headers under tests/ affect only tests, which aren't tidied.
#
# Includes are matched by file name ("x.hpp" and <pptctl/x.hpp> both name
# x.hpp), which over-selects at worst. Standard library only: CI runs it
# with the runner's python3.

import re
import subprocess
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
# A CMakeLists.txt diff line that only adds or drops a list entry.
LIST_ENTRY = re.compile(r"^[+-]\s*(src|tests)/[\w./-]+\.(cpp|hpp)\s*$")
EVERYTHING = re.compile(
    r"^(\.clang-tidy|CMakePresets\.json|cmake/.*|\.github/workflows/ci\.yml|scripts/tidy_files\.py)$"
)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def cmake_lists_change_is_structural(base: str, head: str, path: str) -> bool:
    for line in git("diff", "-U0", base, head, "--", path).splitlines():
        if line.startswith(("+++", "---")) or not line.startswith(("+", "-")):
            continue
        if line[1:].strip() and not LIST_ENTRY.match(line):
            return True
    return False


def includers(headers: set[str], sources: list[Path]) -> set[str]:
    """Source files that include any of `headers` (file names), transitively."""
    includes = {
        str(p): {Path(name).name for name in INCLUDE.findall(p.read_text(errors="replace"))}
        for p in sources
    }
    found: set[str] = set()
    names = set(headers)
    while names:
        reached = {f for f, inc in includes.items() if f not in found and inc & names}
        found |= reached
        names = {Path(f).name for f in reached if f.endswith((".h", ".hpp"))}
    return found


def main() -> None:
    base = sys.argv[1] if len(sys.argv) > 1 else "HEAD^1"
    head = sys.argv[2] if len(sys.argv) > 2 else "HEAD"
    every = sorted(str(p) for p in Path("src").rglob("*.cpp"))
    changed = [f for f in git("diff", "--name-only", base, head).splitlines() if f]

    def everything(reason: str) -> None:
        print(f"{reason}: tidying every file.", file=sys.stderr)
        print("\n".join(every))

    for f in changed:
        if EVERYTHING.match(f):
            return everything(f"{f} changed")
        if Path(f).name == "CMakeLists.txt" and cmake_lists_change_is_structural(base, head, f):
            return everything(f"{f} changed beyond its source lists")

    files = {f for f in changed if f.startswith("src/") and f.endswith(".cpp") and Path(f).exists()}
    headers = {
        Path(f).name for f in changed if f.endswith((".h", ".hpp")) and not f.startswith("tests/")
    }
    if headers:
        scan = [p for p in [*Path("src").rglob("*"), *Path("include").rglob("*")]
                if p.suffix in (".cpp", ".h", ".hpp")]
        files |= {f for f in includers(headers, scan) if f.endswith(".cpp")}

    print(f"Changed headers: {', '.join(sorted(headers)) or '(none)'}", file=sys.stderr)
    print(f"Tidying {len(files)} of {len(every)} files:", file=sys.stderr)
    for f in sorted(files):
        print(f"  {f}", file=sys.stderr)
    print("\n".join(sorted(files)))


if __name__ == "__main__":
    main()
