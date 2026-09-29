#!/usr/bin/env python3
"""Deterministically (re)generate the synthetic .pptx test fixtures.

These fixtures are committed binaries; this script is the source of truth for
how they were built, so reviewers can audit their contents and we can regenerate
them if requirements change. It is dev-only tooling — CMake never invokes it and
the C++ build has no Python dependency.

Run it with `uv` (no install needed):

    uv run --python 3.12 --with python-pptx==1.0.2 tests/fixtures/generate_fixtures.py

Pass --check to verify committed fixtures match regenerated output (useful in CI):

    uv run --python 3.12 --with python-pptx==1.0.2 tests/fixtures/generate_fixtures.py --check

Generates, next to this script:
  - images_deck.pptx        12 slides, each with a distinct embedded PNG.
  - bloat_deck.pptx         2 slides: a 1600x1200 PNG shown 1 in wide, and a small PNG.
  - lint_problems.pptx      7 slides, one per lint rule (plus the printer-settings stub
                            python-pptx ships, which trips the deck-level ctype rule).
  - lint_clean.pptx         2 ordinary slides with the printer-settings stub stripped;
                            the only fixture that lints to zero diagnostics.
  - notes_and_masters.pptx  4 slides with speaker notes, plus a 2nd slide master.
  - tables_deck.pptx        2 slides: a plain 2×2 table and a merged-cell table.
  - charts_deck.pptx        2 slides: a clustered-bar chart and a pie chart.

python-pptx has no API for a second slide master, so notes_and_masters.pptx is
built normally and then post-processed at the package (ZIP/parts) level to inject
a second master with its own layout and theme. Counts asserted by the C++ tests:
12 slides / 12 images, and >= 2 masters + a notesSlide part, respectively.

Reproducibility contract
------------------------
- python-pptx is pinned above so XML serialization is identical across machines.
- All ZIP entries are written with a fixed timestamp and in sorted order so the
  committed bytes are stable regardless of python-pptx's internal entry ordering.
"""

from __future__ import annotations

import argparse
import datetime
import io
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

from pptx import Presentation
from pptx.util import Emu, Inches, Pt

HERE = Path(__file__).resolve().parent

# Fixed metadata so regenerated fixtures don't churn on author/timestamps.
_FIXED_DT = datetime.datetime(2025, 1, 1, 0, 0, 0)
_FIXED_ZIP_DT = (2025, 1, 1, 0, 0, 0)
_REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
_SLIDE_MASTER_REL = _REL_NS + "/slideMaster"


def _png(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    """A minimal solid-color PNG, built without Pillow so the only runtime
    dependency is python-pptx. Distinct colors yield distinct blobs, so
    python-pptx stores each as its own media part."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    row = b"\x00" + bytes(rgb) * width  # filter byte + pixels
    raw = row * height
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def _set_core_props(prs: Presentation, title: str) -> None:
    cp = prs.core_properties
    cp.author = "pptctl fixtures"
    cp.title = title
    cp.created = _FIXED_DT
    cp.modified = _FIXED_DT


_XLSX_DATE_RE = re.compile(
    r"(<dcterms:(?:created|modified)[^>]*>)[^<]*(</dcterms:(?:created|modified)>)"
)
_FIXED_XLSX_DATE = "2025-01-01T00:00:00Z"


def _normalize_xlsx_blob(data: bytes) -> bytes:
    """Normalize an embedded xlsx blob: fix docProps/core.xml timestamps and sort entries."""
    buf = io.BytesIO(data)
    with zipfile.ZipFile(buf) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}
    if "docProps/core.xml" in parts:
        core = _XLSX_DATE_RE.sub(
            lambda m: m.group(1) + _FIXED_XLSX_DATE + m.group(2),
            parts["docProps/core.xml"].decode("utf-8"),
        )
        parts["docProps/core.xml"] = core.encode("utf-8")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in sorted(parts):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, parts[name])
    return out.getvalue()


def _write_zip(path: Path, parts: dict[str, bytes]) -> None:
    """Write parts to a ZIP with fixed timestamps and alphabetically sorted entries.

    Sorting and pinning timestamps are what make regeneration byte-identical
    across machines and python-pptx patch releases that change entry ordering.
    [Content_Types].xml sorts first (ASCII '[' < '_' < letters), matching the
    OOXML convention that renderers expect it at the front of the archive.
    Embedded .xlsx blobs (chart data) are also normalized to remove their
    current-time docProps/core.xml timestamps.
    """
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in sorted(parts):
            data = _normalize_xlsx_blob(parts[name]) if name.endswith(".xlsx") else parts[name]
            info = zipfile.ZipInfo(name, date_time=_FIXED_ZIP_DT)
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data)


def _normalize_zip(path: Path) -> None:
    """Rewrite a .pptx saved by python-pptx with fixed timestamps and sorted entries."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}
    _write_zip(path, parts)


def build_images_deck(path: Path) -> None:
    """12 slides, each carrying one distinct embedded PNG."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    for i in range(12):
        slide = prs.slides.add_slide(blank)
        box = slide.shapes.add_textbox(Inches(0.5), Inches(0.3), Inches(9), Inches(0.8))
        box.text_frame.text = f"Slide {i + 1}"
        # Vary color per slide -> 12 distinct media parts.
        color = ((i * 20) % 256, (i * 35) % 256, (255 - i * 18) % 256)
        png = _png(64 + i, 48 + i, color)
        slide.shapes.add_picture(
            io.BytesIO(png), Inches(1), Inches(1.5), height=Inches(3)
        )
    _set_core_props(prs, "pptctl images fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_bloat_deck(path: Path) -> None:
    """2 slides. Slide 1 carries a 1600x1200 PNG displayed 1 inch wide (about
    1600 DPI effective -> lint img-bloat). Slide 2 carries a 64x48 PNG
    displayed 3 inches tall (about 21 DPI -> clean). Solid colour keeps the
    large PNG a few KB on disk."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    s1 = prs.slides.add_slide(blank)
    s1.shapes.add_picture(
        io.BytesIO(_png(1600, 1200, (200, 40, 40))), Inches(1), Inches(1), width=Inches(1)
    )
    s2 = prs.slides.add_slide(blank)
    s2.shapes.add_picture(
        io.BytesIO(_png(64, 48, (40, 40, 200))), Inches(1), Inches(1), height=Inches(3)
    )
    _set_core_props(prs, "pptctl image bloat fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_lint_problems_deck(path: Path) -> None:
    """One slide per lint rule, each tripping exactly one slide-level rule, in
    rule-registry order. Blank layouts carry no content placeholders, so
    empty-ph can only fire on the Title Only slide. The printer-settings stub
    python-pptx ships is kept: it is the deck-level ctype finding."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    title_only = prs.slide_layouts[5]

    # 1. overflow: text box starting 2 in from the right edge but 4 in wide.
    s = prs.slides.add_slide(blank)
    box = s.shapes.add_textbox(Inches(8), Inches(1), Inches(4), Inches(1))
    box.text_frame.text = "Off the right edge"

    # 2. tiny-text: an 8 pt run.
    s = prs.slides.add_slide(blank)
    box = s.shapes.add_textbox(Inches(1), Inches(1), Inches(6), Inches(1))
    box.text_frame.text = "Fine print"
    box.text_frame.paragraphs[0].runs[0].font.size = Pt(8)

    # 3. alt-text: small picture shown large (no bloat) with its descr removed.
    s = prs.slides.add_slide(blank)
    pic = s.shapes.add_picture(
        io.BytesIO(_png(64, 48, (40, 160, 40))), Inches(1), Inches(1), height=Inches(3)
    )
    pic._element.nvPicPr.cNvPr.attrib.pop("descr", None)

    # 4. font-risk: a face that is neither embedded nor on the safe list.
    s = prs.slides.add_slide(blank)
    box = s.shapes.add_textbox(Inches(1), Inches(1), Inches(6), Inches(1))
    box.text_frame.text = "Custom typeface"
    box.text_frame.paragraphs[0].runs[0].font.name = "Nonexistent Sans"

    # 5. empty-ph: Title Only layout with the title left blank.
    prs.slides.add_slide(title_only)

    # 6. ctype: deck-level, from the retained printerSettings1.bin. A text box
    #    keeps the slide count aligned with the rule list.
    s = prs.slides.add_slide(blank)
    box = s.shapes.add_textbox(Inches(1), Inches(1), Inches(6), Inches(1))
    box.text_frame.text = "Deck-level ctype finding lives in [Content_Types].xml"

    # 7. img-bloat: 1600x1200 PNG shown 1 in wide (about 1600 DPI effective).
    s = prs.slides.add_slide(blank)
    s.shapes.add_picture(
        io.BytesIO(_png(1600, 1200, (200, 40, 40))), Inches(1), Inches(1), width=Inches(1)
    )

    _set_core_props(prs, "pptctl lint problems fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_lint_clean_deck(path: Path) -> None:
    """Two ordinary slides that trip no rule: filled placeholders, theme fonts
    only, a small picture (python-pptx sets descr) shown at a low DPI, and
    everything inside the canvas. strip_printer_settings() runs afterwards."""
    prs = Presentation()
    s = prs.slides.add_slide(prs.slide_layouts[1])  # Title and Content
    s.shapes.title.text = "A clean deck"
    s.placeholders[1].text_frame.text = "Nothing to see here"
    s = prs.slides.add_slide(prs.slide_layouts[6])
    s.shapes.add_picture(
        io.BytesIO(_png(64, 48, (40, 40, 200))), Inches(1), Inches(1), height=Inches(3)
    )
    _set_core_props(prs, "pptctl lint clean fixture")
    prs.save(str(path))
    _normalize_zip(path)


def strip_printer_settings(path: Path) -> None:
    """Remove the printerSettings1.bin stub python-pptx's default template
    carries: the part, its relationship from presentation.xml, and its
    [Content_Types].xml Default entry (the only .bin part in the package)."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}

    stubs = [n for n in parts if n.startswith("ppt/printerSettings/")]
    for n in stubs:
        del parts[n]

    rels = parts["ppt/_rels/presentation.xml.rels"].decode("utf-8")
    rels = re.sub(r'<Relationship [^>]*printerSettings[^>]*/>', "", rels)
    parts["ppt/_rels/presentation.xml.rels"] = rels.encode("utf-8")

    ct = parts["[Content_Types].xml"].decode("utf-8")
    ct = re.sub(r'<Default Extension="bin" [^>]*/>', "", ct)
    parts["[Content_Types].xml"] = ct.encode("utf-8")

    _write_zip(path, parts)


def build_notes_deck_base(path: Path) -> None:
    """4 slides, each with speaker notes (single master at this stage)."""
    prs = Presentation()
    layout = prs.slide_layouts[1]  # Title and Content
    for i in range(4):
        slide = prs.slides.add_slide(layout)
        slide.shapes.title.text = f"Topic {i + 1}"
        body = slide.placeholders[1].text_frame
        body.text = f"Body text for topic {i + 1}."
        notes = slide.notes_slide.notes_text_frame
        notes.text = f"Speaker notes for slide {i + 1}: remember to elaborate here."
    _set_core_props(prs, "pptctl notes + masters fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_tables_deck(path: Path) -> None:
    """2 slides: a plain 2x2 table (bold header), and a table whose top row is
    merged across both columns."""
    prs = Presentation()
    blank = prs.slide_layouts[6]

    # Slide 1: a 2x2 table with a bold header row.
    s1 = prs.slides.add_slide(blank)
    t1 = s1.shapes.add_table(2, 2, Inches(1), Inches(1), Inches(6), Inches(2)).table
    for col, header in enumerate(("Quarter", "Revenue")):
        cell = t1.cell(0, col)
        cell.text = header
        cell.text_frame.paragraphs[0].runs[0].font.bold = True
    t1.cell(1, 0).text = "Q1"
    t1.cell(1, 1).text = "100"

    # Slide 2: a 2x2 table with the top row merged across both columns.
    s2 = prs.slides.add_slide(blank)
    t2 = s2.shapes.add_table(2, 2, Inches(1), Inches(1), Inches(6), Inches(2)).table
    t2.cell(0, 0).merge(t2.cell(0, 1))
    t2.cell(0, 0).text = "Annual Summary"
    t2.cell(1, 0).text = "Left"
    t2.cell(1, 1).text = "Right"

    _set_core_props(prs, "pptctl tables fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_charts_deck(path: Path) -> None:
    """2 slides: a clustered-bar chart (2 series, 3 categories) and a pie chart
    (1 series, 3 categories). Both have titles so the C++ tests can verify title
    extraction. Data is written inline by python-pptx as numCache/strCache."""
    from pptx.chart.data import ChartData
    from pptx.enum.chart import XL_CHART_TYPE

    prs = Presentation()
    blank = prs.slide_layouts[6]

    # Slide 1: clustered bar, 2 series x 3 categories.
    s1 = prs.slides.add_slide(blank)
    cd1 = ChartData()
    cd1.categories = ["Q1", "Q2", "Q3"]
    cd1.add_series("Revenue", (100, 200, 150))
    cd1.add_series("Cost", (80, 120, 90))
    chart1 = s1.shapes.add_chart(
        XL_CHART_TYPE.BAR_CLUSTERED,
        Inches(1), Inches(1), Inches(6), Inches(4),
        cd1,
    ).chart
    chart1.has_title = True
    chart1.chart_title.text_frame.text = "Revenue vs Cost"

    # Slide 2: pie, 1 series x 3 categories.
    s2 = prs.slides.add_slide(blank)
    cd2 = ChartData()
    cd2.categories = ["Alpha", "Beta", "Gamma"]
    cd2.add_series("Share", (30, 50, 20))
    chart2 = s2.shapes.add_chart(
        XL_CHART_TYPE.PIE,
        Inches(1), Inches(1), Inches(6), Inches(4),
        cd2,
    ).chart
    chart2.has_title = True
    chart2.chart_title.text_frame.text = "Market Share"

    _set_core_props(prs, "pptctl charts fixture")
    prs.save(str(path))
    _normalize_zip(path)


def _next_index(names: list[str], pattern: str) -> int:
    """Highest numeric suffix matching `pattern` (one capture group), plus 1."""
    rx = re.compile(pattern)
    nums = [int(m.group(1)) for n in names if (m := rx.fullmatch(n))]
    return (max(nums) + 1) if nums else 1


def inject_second_master(path: Path) -> None:
    """Add a second slide master (its own layout + theme) by duplicating the
    first at the package level. python-pptx exposes no API for this."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}

    names = list(parts)
    master_i = _next_index(names, r"ppt/slideMasters/slideMaster(\d+)\.xml")
    layout_i = _next_index(names, r"ppt/slideLayouts/slideLayout(\d+)\.xml")
    theme_i = _next_index(names, r"ppt/theme/theme(\d+)\.xml")

    master_part = f"ppt/slideMasters/slideMaster{master_i}.xml"
    layout_part = f"ppt/slideLayouts/slideLayout{layout_i}.xml"
    theme_part = f"ppt/theme/theme{theme_i}.xml"

    # New theme + layout are byte-copies of the originals.
    parts[theme_part] = parts["ppt/theme/theme1.xml"]
    parts[layout_part] = parts["ppt/slideLayouts/slideLayout1.xml"]
    parts[f"ppt/slideLayouts/_rels/slideLayout{layout_i}.xml.rels"] = (
        b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        b'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        b'<Relationship Id="rId1" Type="' + _REL_NS.encode() + b'/slideMaster" '
        b'Target="../slideMasters/slideMaster' + str(master_i).encode() + b'.xml"/>'
        b"</Relationships>"
    )

    # The new master reuses master1's body but points its layout list at the
    # single duplicated layout (rId1); its theme is rId2 in the master's rels.
    master_xml = parts["ppt/slideMasters/slideMaster1.xml"].decode("utf-8")
    master_xml = re.sub(
        r"<p:sldLayoutIdLst>.*?</p:sldLayoutIdLst>",
        '<p:sldLayoutIdLst><p:sldLayoutId id="2147483800" r:id="rId1"/></p:sldLayoutIdLst>',
        master_xml,
        flags=re.S,
    )
    parts[master_part] = master_xml.encode("utf-8")
    parts[f"ppt/slideMasters/_rels/slideMaster{master_i}.xml.rels"] = (
        b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        b'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        b'<Relationship Id="rId1" Type="' + _REL_NS.encode() + b'/slideLayout" '
        b'Target="../slideLayouts/slideLayout' + str(layout_i).encode() + b'.xml"/>'
        b'<Relationship Id="rId2" Type="' + _REL_NS.encode() + b'/theme" '
        b'Target="../theme/theme' + str(theme_i).encode() + b'.xml"/>'
        b"</Relationships>"
    )

    # Register the master in presentation.xml.rels (fresh, non-colliding rId).
    rels = parts["ppt/_rels/presentation.xml.rels"].decode("utf-8")
    used = [int(m) for m in re.findall(r'Id="rId(\d+)"', rels)]
    new_rid = f"rId{(max(used) + 1) if used else 1}"
    rels = rels.replace(
        "</Relationships>",
        f'<Relationship Id="{new_rid}" Type="{_SLIDE_MASTER_REL}" '
        f'Target="slideMasters/slideMaster{master_i}.xml"/></Relationships>',
    )
    parts["ppt/_rels/presentation.xml.rels"] = rels.encode("utf-8")

    # Add the master to the presentation's sldMasterIdLst.
    pres = parts["ppt/presentation.xml"].decode("utf-8")
    pres = pres.replace(
        "</p:sldMasterIdLst>",
        f'<p:sldMasterId id="2147483700" r:id="{new_rid}"/></p:sldMasterIdLst>',
    )
    parts["ppt/presentation.xml"] = pres.encode("utf-8")

    # Declare the three new parts in [Content_Types].xml.
    ct = parts["[Content_Types].xml"].decode("utf-8")
    base = "application/vnd.openxmlformats-officedocument"
    overrides = (
        f'<Override PartName="/{master_part}" '
        f'ContentType="{base}.presentationml.slideMaster+xml"/>'
        f'<Override PartName="/{layout_part}" '
        f'ContentType="{base}.presentationml.slideLayout+xml"/>'
        f'<Override PartName="/{theme_part}" ContentType="{base}.theme+xml"/>'
    )
    ct = ct.replace("</Types>", overrides + "</Types>")
    parts["[Content_Types].xml"] = ct.encode("utf-8")

    _write_zip(path, parts)


_FIXTURES: list[tuple[str, list[object]]] = [
    ("images_deck.pptx", [build_images_deck]),
    ("bloat_deck.pptx", [build_bloat_deck]),
    ("lint_problems.pptx", [build_lint_problems_deck]),
    ("lint_clean.pptx", [build_lint_clean_deck, strip_printer_settings]),
    ("notes_and_masters.pptx", [build_notes_deck_base, inject_second_master]),
    ("tables_deck.pptx", [build_tables_deck]),
    ("charts_deck.pptx", [build_charts_deck]),
]


def _build_all(dest: Path) -> None:
    for name, builders in _FIXTURES:
        p = dest / name
        for builder in builders:
            builder(p)  # type: ignore[operator]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate pptctl synthetic test fixtures."
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Regenerate to a temp dir and exit 1 if any committed fixture differs.",
    )
    args = parser.parse_args()

    if args.check:
        dirty: list[str] = []
        with tempfile.TemporaryDirectory() as tmp:
            _build_all(Path(tmp))
            for name, _ in _FIXTURES:
                committed = (HERE / name).read_bytes()
                generated = (Path(tmp) / name).read_bytes()
                if committed != generated:
                    dirty.append(name)
        if dirty:
            for name in dirty:
                print(f"STALE: {name}", file=sys.stderr)
            sys.exit(1)
        print("All fixtures are up-to-date.")
    else:
        _build_all(HERE)
        names = ", ".join(n for n, _ in _FIXTURES)
        print(f"wrote {names} to {HERE}")


if __name__ == "__main__":
    main()
