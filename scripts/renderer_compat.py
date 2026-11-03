#!/usr/bin/env python3
"""Renderer-compatibility check of `optimize` and `merge` output (#173).

    scripts/renderer_compat.py fetch [DIR]          download the corpus, verify sha256
    scripts/renderer_compat.py run BUILD_DIR [DIR] [--out OUT] [--doc FILE]

The corpus is tests/corpus/manifest.json: openly licensed decks downloaded
into DIR (default tests/corpus/decks/, gitignored) plus a few committed ones.

`run` converts every source deck to PDF with headless LibreOffice as the
baseline, then for each deck `pptctl optimize`, and for consecutive pairs
`pptctl merge` (each deck keeping its formatting, and with --theme=1). Every
output is checked three ways:

  integrity  `pptctl lint --only integrity` finds no more than in its sources
             (real decks often arrive with an orphan part or two);
  converts   LibreOffice writes a PDF (it can exit 0 without one, so the PDF is
             what counts);
  pages      the PDF has as many pages as the source PDFs together. Compared
             with the source's PDF rather than the slide count because
             LibreOffice leaves hidden slides out of the PDF.

A deck whose own source doesn't convert is reported but not judged. Results go
to OUT/results.md and OUT/results.json, to $GITHUB_STEP_SUMMARY when set, and
with --doc to a full Markdown page (docs/renderer-compat.md). Exit 1 if any
check failed.

Needs python3 (stdlib only), LibreOffice (`soffice`, or $SOFFICE) and pdfinfo
(poppler-utils).
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests" / "corpus" / "manifest.json"
DEFAULT_DIR = ROOT / "tests" / "corpus" / "decks"
CONVERT_TIMEOUT = 300


def load_manifest():
    return json.loads(MANIFEST.read_text())["decks"]


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def deck_path(deck, corpus):
    if "path" in deck:
        return ROOT / deck["path"]
    return corpus / f"{deck['name']}.pptx"


# --- fetch -------------------------------------------------------------------


def fetch(corpus):
    corpus.mkdir(parents=True, exist_ok=True)
    bad = 0
    for deck in load_manifest():
        path = deck_path(deck, corpus)
        if path.exists() and sha256(path) == deck["sha256"]:
            print(f"ok (cached): {deck['name']}")
            continue
        if "url" not in deck:
            print(f"FAIL: {deck['name']}: {path} is missing or changed", file=sys.stderr)
            bad += 1
            continue
        tmp = path.with_suffix(".part")
        try:
            req = urllib.request.Request(deck["url"], headers={"User-Agent": "pptctl-corpus"})
            with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            print(f"FAIL: {deck['name']}: download failed: {e}", file=sys.stderr)
            tmp.unlink(missing_ok=True)
            bad += 1
            continue
        if sha256(tmp) != deck["sha256"]:
            print(f"FAIL: {deck['name']}: sha256 mismatch", file=sys.stderr)
            tmp.unlink()
            bad += 1
            continue
        tmp.replace(path)
        print(f"ok: {deck['name']}")
    return 1 if bad else 0


# --- run ---------------------------------------------------------------------


class Tools:
    def __init__(self, build, work):
        exe = "pptctl.exe" if os.name == "nt" else "pptctl"
        self.pptctl = str(pathlib.Path(build).resolve() / exe)
        self.soffice = (
            os.environ.get("SOFFICE")
            or shutil.which("soffice")
            or shutil.which("libreoffice")
            or "/Applications/LibreOffice.app/Contents/MacOS/soffice"
        )
        # A private profile: no clash with a running LibreOffice, no first-run state.
        self.profile = (work / "lo-profile").resolve().as_uri()

    def version(self, cmd):
        out = subprocess.run(cmd, capture_output=True, text=True, check=False).stdout.strip()
        return out.splitlines()[0] if out else "unknown"

    def run(self, *args):
        return subprocess.run(
            [self.pptctl, *map(str, args)], capture_output=True, text=True, check=False
        )

    def integrity(self, path):
        """Integrity findings of a deck (errors + warnings), or None if unreadable."""
        r = self.run("lint", "--json", "--only", "integrity", path)
        try:
            s = json.loads(r.stdout)["summary"]
        except (json.JSONDecodeError, KeyError):
            return None
        return s["errors"] + s["warnings"]

    def pages(self, path, outdir):
        """Pages of LibreOffice's PDF of a deck, or None when there is no PDF."""
        outdir.mkdir(parents=True, exist_ok=True)
        try:
            subprocess.run(
                [
                    self.soffice,
                    f"-env:UserInstallation={self.profile}",
                    "--headless",
                    "--convert-to",
                    "pdf",
                    "--outdir",
                    str(outdir),
                    str(path),
                ],
                capture_output=True,
                timeout=CONVERT_TIMEOUT,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return None
        pdf = outdir / (pathlib.Path(path).stem + ".pdf")
        if not pdf.exists():
            return None
        info = subprocess.run(
            ["pdfinfo", str(pdf)], capture_output=True, text=True, check=False
        ).stdout
        m = re.search(r"^Pages:\s+(\d+)", info, re.MULTILINE)
        pdf.unlink()
        return int(m.group(1)) if m else None


def slide_count(path):
    with zipfile.ZipFile(path) as z:
        return sum(1 for n in z.namelist() if re.fullmatch(r"ppt/slides/slide\d+\.xml", n))


def judge(rc, lint, lint_src, pages, pages_src):
    """(verdict, notes) for one output."""
    if pages_src is None:
        return "n/a", "source doesn't convert"
    problems = []
    if rc != 0:
        problems.append(f"pptctl exit {rc}")
    if lint is None:
        problems.append("lint can't read output")
    elif lint > lint_src:
        problems.append(f"integrity {lint_src} -> {lint}")
    if pages is None:
        problems.append("no PDF")
    elif pages != pages_src:
        problems.append(f"{pages} pages, expected {pages_src}")
    return ("FAIL", "; ".join(problems)) if problems else ("pass", "")


def kib(n):
    return f"{n / 1024:,.0f} KiB" if n < 1024 * 1024 else f"{n / 1048576:.1f} MiB"


def run(build, corpus, out, doc):
    out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="pptctl-compat-"))
    tools = Tools(build, work)
    decks = load_manifest()
    missing = [d["name"] for d in decks if not deck_path(d, corpus).exists()]
    if missing:
        sys.exit(f"missing decks (run fetch first): {', '.join(missing)}")

    meta = {
        "date": datetime.date.today().isoformat(),
        "libreoffice": tools.version([tools.soffice, "--version"]),
        "pptctl": tools.version([tools.pptctl, "--version"]),
    }
    print(f"{meta['pptctl']} / {meta['libreoffice']}", flush=True)

    src = {}
    for d in decks:
        path = deck_path(d, corpus)
        src[d["name"]] = {
            "path": path,
            "slides": slide_count(path),
            "bytes": path.stat().st_size,
            "integrity": tools.integrity(path),
            "pages": tools.pages(path, work / "pdf"),
        }
        print(f"source {d['name']}: {src[d['name']]['pages']} pages", flush=True)

    optimize = []
    for d in decks:
        s = src[d["name"]]
        dst = work / f"opt-{d['name']}.pptx"
        r = tools.run("optimize", "-o", dst, s["path"])
        lint = tools.integrity(dst) if dst.exists() else None
        pages = tools.pages(dst, work / "pdf") if dst.exists() else None
        verdict, notes = judge(r.returncode, lint, s["integrity"], pages, s["pages"])
        optimize.append(
            {
                "deck": d["name"],
                "producer": d["producer"],
                "slides": s["slides"],
                "bytes_in": s["bytes"],
                "bytes_out": dst.stat().st_size if dst.exists() else None,
                "integrity_in": s["integrity"],
                "integrity_out": lint,
                "pages_in": s["pages"],
                "pages_out": pages,
                "verdict": verdict,
                "notes": notes,
            }
        )
        print(f"optimize {d['name']}: {verdict} {notes}", flush=True)

    merges = []
    names = [d["name"] for d in decks]
    pairs = [(names[i], names[(i + 1) % len(names)]) for i in range(0, len(names), 2)]
    for a, b in pairs:
        for theme in (None, 1):
            dst = work / f"merge-{a}-{b}-{theme or 'keep'}.pptx"
            args = ["merge", src[a]["path"], src[b]["path"], "-o", dst]
            if theme:
                args.append(f"--theme={theme}")
            r = tools.run(*args)
            lint = tools.integrity(dst) if dst.exists() else None
            pages = tools.pages(dst, work / "pdf") if dst.exists() else None
            want = (
                None
                if src[a]["pages"] is None or src[b]["pages"] is None
                else (src[a]["pages"] + src[b]["pages"])
            )
            lint_src = (src[a]["integrity"] or 0) + (src[b]["integrity"] or 0)
            verdict, notes = judge(r.returncode, lint, lint_src, pages, want)
            merges.append(
                {
                    "a": a,
                    "b": b,
                    "theme": theme,
                    "integrity_in": lint_src,
                    "integrity_out": lint,
                    "pages_expected": want,
                    "pages_out": pages,
                    "verdict": verdict,
                    "notes": notes,
                }
            )
            print(
                f"merge {a} + {b} ({'--theme=1' if theme else 'keep'}): {verdict} {notes}",
                flush=True,
            )

    results = {**meta, "optimize": optimize, "merge": merges}
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    table = report(results)
    (out / "results.md").write_text(table)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(table)
    if doc:
        pathlib.Path(doc).write_text(page(results, table, decks))
    shutil.rmtree(work, ignore_errors=True)
    failed = [r for r in optimize + merges if r["verdict"] == "FAIL"]
    print(f"{len(failed)} failure(s)")
    return 1 if failed else 0


def cell(v):
    return "–" if v is None else str(v).replace("|", "\\|")


def report(res):
    opt, mer = res["optimize"], res["merge"]
    n_pass = sum(r["verdict"] == "pass" for r in opt + mer)
    n_fail = sum(r["verdict"] == "FAIL" for r in opt + mer)
    n_na = sum(r["verdict"] == "n/a" for r in opt + mer)
    lines = [
        "## Renderer compatibility (LibreOffice)",
        "",
        (
            f"{res['pptctl']}, {res['libreoffice']}, {res['date']}: "
            f"**{n_pass} pass, {n_fail} fail, {n_na} not judged** "
            f"({len(opt)} optimized decks, {len(mer)} merges)."
        ),
        "",
        "### optimize",
        "",
        "| Deck | Producer | Slides | Size | Integrity findings | PDF pages | Result |",
        "|---|---|---:|---|---|---|---|",
    ]
    for r in opt:
        size = (
            f"{kib(r['bytes_in'])} → {kib(r['bytes_out'])}"
            if r["bytes_out"]
            else kib(r["bytes_in"])
        )
        lines.append(
            f"| {r['deck']} | {cell(r['producer'])} | {r['slides']} | {size} | "
            f"{cell(r['integrity_in'])} → {cell(r['integrity_out'])} | "
            f"{cell(r['pages_in'])} → {cell(r['pages_out'])} | "
            f"{r['verdict']}{': ' + r['notes'] if r['notes'] else ''} |"
        )
    lines += [
        "",
        "### merge",
        "",
        "| Decks | Formatting | Integrity findings | PDF pages | Result |",
        "|---|---|---|---|---|",
    ]
    for r in mer:
        fmt = "`--theme=1`" if r["theme"] else "each keeps its own"
        lines.append(
            f"| {r['a']} + {r['b']} | {fmt} | {cell(r['integrity_in'])} → "
            f"{cell(r['integrity_out'])} | {cell(r['pages_expected'])} → "
            f"{cell(r['pages_out'])} | "
            f"{r['verdict']}{': ' + r['notes'] if r['notes'] else ''} |"
        )
    return "\n".join(lines) + "\n"


def page(res, table, decks):
    corpus = ["| Deck | Producer | Licence | Source |", "|---|---|---|---|"]
    for d in decks:
        who = "; ".join(d["creators"])
        src = (
            f"[{cell(d['title'])}]({d['source']}), {cell(who)}"
            if "source" in d
            else f"{cell(d['title'])} (`{d['path']}`)"
        )
        corpus.append(f"| {d['name']} | {cell(d['producer'])} | {d['licence']} | {src} |")
    return f"""# Renderer compatibility: LibreOffice

`optimize` and `merge` rewrite decks, and every deck they write has to open
without complaint in PowerPoint, Keynote, LibreOffice and Google Slides. Of
those four, LibreOffice is the only one that runs headless in CI, so it gets
a repeatable check: `.github/workflows/renderer-compat.yml` runs
[`scripts/renderer_compat.py`](../scripts/renderer_compat.py) weekly, by hand,
and on pull requests that touch `optimize` or `merge`. This page is its
latest result, regenerated with:

```bash
python3 scripts/renderer_compat.py fetch
python3 scripts/renderer_compat.py run build/macos-release --doc docs/renderer-compat.md
```

Each deck in the corpus below is optimized, and consecutive pairs are merged
twice: once with each deck keeping its own formatting, once with `--theme=1`.
An output passes when:

- **integrity:** `pptctl lint --only integrity` finds no more problems in it
  than in its sources;
- **converts:** LibreOffice writes a PDF of it (LibreOffice can exit 0 without
  writing one, so the PDF is what is checked);
- **pages:** the PDF has as many pages as the PDFs of its sources together.
  The source's PDF is the reference rather than the slide count because
  LibreOffice leaves hidden slides out.

A deck whose source LibreOffice can't convert is listed but not judged.

**What this does not check.** It shows a deck loads and converts, not that it
looks the same: a picture drawn in the wrong place, a lost colour or a
substituted font all still pass. Nor does it see the repair prompt PowerPoint
shows or a deck Keynote refuses. Those checks stay manual: see
[the optimize validation](phase5-optimize-validation.md) and
[the merge validation](phase6-merge-validation.md).

{table.replace("## Renderer compatibility (LibreOffice)", "## Latest result", 1)}
## Corpus

`tests/corpus/manifest.json` lists the decks with their download URL and
SHA-256; `fetch` verifies every download. The decks from Zenodo are not
committed. The Keynote export and the python-pptx fixtures are in the
repository.

{chr(10).join(corpus)}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("dir", nargs="?", type=pathlib.Path, default=DEFAULT_DIR)
    r = sub.add_parser("run")
    r.add_argument("build", type=pathlib.Path)
    r.add_argument("dir", nargs="?", type=pathlib.Path, default=DEFAULT_DIR)
    r.add_argument("--out", type=pathlib.Path, default=pathlib.Path("renderer-compat"))
    r.add_argument("--doc", type=pathlib.Path)
    a = ap.parse_args()
    if a.cmd == "fetch":
        return fetch(a.dir)
    return run(a.build, a.dir, a.out, a.doc)


if __name__ == "__main__":
    sys.exit(main())
