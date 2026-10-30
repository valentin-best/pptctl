#!/usr/bin/env python3
"""Deterministically (re)generate the synthetic .pptx test fixtures.

These fixtures are committed binaries; this script is the source of truth for
how they were built, so reviewers can audit their contents and we can regenerate
them if requirements change. It is dev-only tooling — CMake never invokes it and
the C++ build has no Python dependency.

Run it with `uv` (no install needed):

    uv run --python 3.12 --with python-pptx==1.0.2 --with pillow==13.0.0 tests/fixtures/generate_fixtures.py

Pass --check to verify committed fixtures match regenerated output (useful in CI):

    uv run --python 3.12 --with python-pptx==1.0.2 --with pillow==13.0.0 tests/fixtures/generate_fixtures.py --check

Generates, next to this script:
  - images_deck.pptx        12 slides, each with a distinct embedded PNG.
  - images_deck_v2.pptx     images_deck.pptx with one picture replaced, one removed,
                            one added and one alt text changed; media parts renamed.
  - bloat_deck.pptx         3 slides: tiny-but-oversampled PNG, large oversampled PNG, clean PNG.
  - crop_deck.pptx          2 slides: a cropped PNG; a PNG shown twice with different crops.
  - optimize_deck.pptx      3 slides with something for every optimize pass (#105): an
                            oversized JPEG, an oversized PNG with alpha, a cropped JPEG; plus
                            a second master with an unused layout, an orphan media part and
                            an embedded font.
  - lint_problems.pptx      7 slides, one per lint rule (plus the printer-settings stub
                            python-pptx ships, which trips the deck-level ctype rule).
  - lint_clean.pptx         2 ordinary slides with the printer-settings stub stripped;
                            the only fixture that lints to zero diagnostics.
  - notes_and_masters.pptx  4 slides with speaker notes, plus a 2nd slide master.
  - tables_deck.pptx        2 slides: a plain 2×2 table and a merged-cell table.
  - charts_deck.pptx        2 slides: a clustered-bar chart and a pie chart.
  - diff_base.pptx          6 slides for the diff goldens (#75): title, two bullet
                            slides, a solid-filled rectangle, speaker notes, a 3x3 table.
  - diff_text_edit.pptx     diff_base with one word changed, one bullet added, one
                            bullet deleted, one run made bold and the notes edited.
  - diff_reorder.pptx       diff_base with slides 2 and 4 swapped.
  - diff_slide_added.pptx   diff_base with a new slide inserted at position 3.
  - diff_slide_removed.pptx diff_base with slide 4 removed.
  - diff_shape_edit.pptx    diff_base with the rectangle moved, resized and recolored,
                            and a text box nudged by 1 EMU (below the tolerance).
  - diff_table_edit.pptx    diff_base with one table cell edited and one row added.
  - charts_deck_v2.pptx     charts_deck.pptx with one value changed, one series added
                            and one chart title changed.
  - merge_a.pptx            3 slides on the default template (title, bullets, picture),
                            two of them showing the shared logo PNG.
  - merge_b.pptx            2 slides on a restyled theme (other colors and fonts), both
                            with speaker notes; slide 2 holds a chart. Two sections,
                            "Intro" and "Data", one slide each.
  - merge_c.pptx            2 slides on merge_a's template, showing the same logo PNG
                            (byte-identical, for media de-duplication); an external
                            hyperlink, and a picture on slide 2 linking to slide 1.

python-pptx has no API for a second slide master, so notes_and_masters.pptx is
built normally and then post-processed at the package (ZIP/parts) level to inject
a second master with its own layout and theme. Counts asserted by the C++ tests:
12 slides / 12 images, and >= 2 masters + a notesSlide part, respectively.

Reproducibility contract
------------------------
- python-pptx is pinned above so XML serialization is identical across machines,
  and Pillow (its own dependency) so the one JPEG encoder in use is too.
- All ZIP entries are written with a fixed timestamp and in sorted order so the
  committed bytes are stable regardless of python-pptx's internal entry ordering.
"""

from __future__ import annotations

import argparse
import datetime
import functools
import io
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
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


def _noise_png(width: int, height: int, seed: int) -> bytes:
    """An incompressible RGB PNG from a fixed LCG, stored with zlib level 0 so
    the bytes are identical across zlib versions. ~3 bytes/pixel on disk: use
    it where a fixture must exceed lint's img-bloat stored-size floor."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    state = seed & 0xFFFFFFFF
    pixels = bytearray()
    for _ in range(width * height * 3):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        pixels.append((state >> 16) & 0xFF)
    rows = b"".join(
        b"\x00" + bytes(pixels[y * width * 3 : (y + 1) * width * 3]) for y in range(height)
    )
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(rows, 0))
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
        png = _png(64 + i, 48 + i, _images_deck_color(i))
        slide.shapes.add_picture(
            io.BytesIO(png), Inches(1), Inches(1.5), height=Inches(3)
        )
    _set_core_props(prs, "pptctl images fixture")
    prs.save(str(path))
    _normalize_zip(path)


def _images_deck_color(i: int) -> tuple[int, int, int]:
    return ((i * 20) % 256, (i * 35) % 256, (255 - i * 18) % 256)


def build_images_deck_v2(path: Path) -> None:
    """images_deck.pptx with four picture edits, for the picture diff (#73):
      slide 3   picture replaced by a different PNG (same shape id/name)
      slide 5   picture removed
      slide 7   a second picture added
      slide 9   alt text changed (same media)
    Every other slide is byte-for-byte the same content as images_deck.pptx.
    rename_media_parts() then renames every media part so identical bytes live
    under different part names in the two decks: the diff must match them by
    content hash, not by name."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    for i in range(12):
        slide = prs.slides.add_slide(blank)
        box = slide.shapes.add_textbox(Inches(0.5), Inches(0.3), Inches(9), Inches(0.8))
        box.text_frame.text = f"Slide {i + 1}"
        color = _images_deck_color(i)
        if i == 2:
            color = (250, 250, 250)  # unused by the base deck: a new media part
        if i == 4:
            continue  # picture removed
        png = _png(64 + i, 48 + i, color)
        pic = slide.shapes.add_picture(
            io.BytesIO(png), Inches(1), Inches(1.5), height=Inches(3)
        )
        if i == 6:
            slide.shapes.add_picture(
                io.BytesIO(_png(96, 72, (10, 250, 10))), Inches(5), Inches(1.5), height=Inches(3)
            )
        if i == 8:
            pic._element.nvPicPr.cNvPr.set("descr", "Slide 9 picture, described")
    _set_core_props(prs, "pptctl images fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_charts_deck_v2(path: Path) -> None:
    """charts_deck.pptx with three chart edits, for the chart diff (#74):
      slide 1   Revenue Q2 200 -> 220, and a third series "Profit" added
      slide 2   the pie chart's title changed
    Chart types, shape ids/names, geometry and categories are unchanged, so
    the only ops a diff should report are chart ops."""
    from pptx.chart.data import ChartData
    from pptx.enum.chart import XL_CHART_TYPE

    prs = Presentation()
    blank = prs.slide_layouts[6]

    s1 = prs.slides.add_slide(blank)
    cd1 = ChartData()
    cd1.categories = ["Q1", "Q2", "Q3"]
    cd1.add_series("Revenue", (100, 220, 150))
    cd1.add_series("Cost", (80, 120, 90))
    cd1.add_series("Profit", (20, 100, 60))
    chart1 = s1.shapes.add_chart(
        XL_CHART_TYPE.BAR_CLUSTERED,
        Inches(1), Inches(1), Inches(6), Inches(4),
        cd1,
    ).chart
    chart1.has_title = True
    chart1.chart_title.text_frame.text = "Revenue vs Cost"

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
    chart2.chart_title.text_frame.text = "Market Share 2025"

    _set_core_props(prs, "pptctl charts fixture")
    prs.save(str(path))
    _normalize_zip(path)


def rename_media_parts(path: Path) -> None:
    """Rename ppt/media/imageN.png -> ppt/media/pictureN.png and rewrite every
    relationship target that points at one. [Content_Types].xml maps media by
    extension (Default), so it needs no change."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}

    renames = {
        n: n.replace("ppt/media/image", "ppt/media/picture")
        for n in parts
        if n.startswith("ppt/media/image")
    }
    for old, new in renames.items():
        parts[new] = parts.pop(old)
    for n in [n for n in parts if n.endswith(".rels")]:
        rels = parts[n].decode("utf-8")
        for old, new in renames.items():
            rels = rels.replace(old.removeprefix("ppt/"), new.removeprefix("ppt/"))
        parts[n] = rels.encode("utf-8")

    _write_zip(path, parts)


_DIFF_VARIANTS = (
    "text_edit",
    "reorder",
    "slide_added",
    "slide_removed",
    "shape_edit",
    "table_edit",
)


def build_diff_deck(path: Path, variant: str | None = None) -> None:
    """The diff golden pairs (#75): one base deck and six one-change variants,
    all from this one builder so everything the variant does not touch is the
    same content byte-for-byte (slide part names shift when a slide is skipped
    or appended, which the diff must not care about).

    diff_base.pptx, 6 slides:
      1  title slide                                   "Quarterly review"
      2  Title and Content, 4 bullets + a source text box at explicit geometry
      3  Title and Content, 3 bullets
      4  blank, a rectangle with a solid #4472C4 fill at (1, 1) in, 3 x 2 in
      5  Title and Content, 2 bullets, with speaker notes
      6  blank, a 3x3 table

    Variants (exactly one edit family each):
      text_edit      slide 2: "last" -> "this" in bullet 2, a 5th bullet added;
                     slide 3: bullet 1 made bold, bullet 3 deleted;
                     slide 5: one word of the notes changed
      reorder        slides 2 and 4 swapped in the sldIdLst, nothing else
      slide_added    a new bullet slide inserted at position 3
      slide_removed  slide 4 (the rectangle) skipped
      shape_edit     slide 4: rectangle moved by +0.5 in on both axes, resized to
                     4 x 2.5 in, recolored to #ED7D31; slide 2: the source text
                     box nudged right by exactly 1 EMU, which is far below the
                     default geometry tolerance (1 px) and must not be reported
      table_edit     slide 6: cell (2, 3) "20" -> "25", a 4th row added
    """
    if variant is not None and variant not in _DIFF_VARIANTS:
        raise ValueError(f"unknown diff variant {variant!r}")

    prs = Presentation()
    title_layout = prs.slide_layouts[0]
    content_layout = prs.slide_layouts[1]
    blank = prs.slide_layouts[6]

    # 1. Title slide.
    s = prs.slides.add_slide(title_layout)
    s.shapes.title.text = "Quarterly review"
    s.placeholders[1].text = "pptctl diff fixture"

    # 2. Agenda: 4 bullets and a source line in a text box with explicit geometry.
    agenda = [
        "Welcome and introductions",
        "Review of last quarter",
        "Roadmap for next quarter",
        "Questions and answers",
    ]
    if variant == "text_edit":
        agenda[1] = "Review of this quarter"
        agenda.append("Closing remarks")
    s = prs.slides.add_slide(content_layout)
    s.shapes.title.text = "Agenda"
    _fill_bullets(s.placeholders[1].text_frame, agenda)
    source_left = Inches(0.5) + (1 if variant == "shape_edit" else 0)
    box = s.shapes.add_textbox(Emu(source_left), Inches(6.6), Inches(9), Inches(0.5))
    box.text_frame.text = "Source: internal reporting"

    # 3. Results: 3 bullets.
    results = ["Revenue up twelve percent", "Costs flat", "Two new hires"]
    if variant == "text_edit":
        results.pop()  # "Two new hires" deleted
    s = prs.slides.add_slide(content_layout)
    s.shapes.title.text = "Results"
    _fill_bullets(s.placeholders[1].text_frame, results)
    if variant == "text_edit":
        s.placeholders[1].text_frame.paragraphs[0].runs[0].font.bold = True

    # 4. A rectangle with an explicit solid fill and known geometry.
    if variant != "slide_removed":
        s = prs.slides.add_slide(blank)
        if variant == "shape_edit":
            geom = (Inches(1.5), Inches(1.5), Inches(4), Inches(2.5))
            color = RGBColor(0xED, 0x7D, 0x31)
        else:
            geom = (Inches(1), Inches(1), Inches(3), Inches(2))
            color = RGBColor(0x44, 0x72, 0xC4)
        rect = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, *geom)
        rect.fill.solid()
        rect.fill.fore_color.rgb = color
        rect.text_frame.text = "Key figure"

    # 5. Timeline with speaker notes.
    s = prs.slides.add_slide(content_layout)
    s.shapes.title.text = "Timeline"
    _fill_bullets(s.placeholders[1].text_frame, ["Kick-off in May", "Launch in September"])
    notes = "Remember to pause for questions."
    if variant == "text_edit":
        notes = "Remember to pause for discussion."
    s.notes_slide.notes_text_frame.text = notes

    # 6. A 3x3 table (4 rows in table_edit).
    grid = [
        ["Region", "Q1", "Q2"],
        ["North", "10", "20"],
        ["South", "30", "40"],
    ]
    if variant == "table_edit":
        grid[1][2] = "25"
        grid.append(["East", "50", "60"])
    s = prs.slides.add_slide(blank)
    table = s.shapes.add_table(
        len(grid), 3, Inches(1), Inches(1), Inches(6), Inches(0.5) * len(grid)
    ).table
    for r, row in enumerate(grid):
        for c, text in enumerate(row):
            table.cell(r, c).text = text

    # 7. Only slide_added: a new slide, then moved to position 3.
    if variant == "slide_added":
        s = prs.slides.add_slide(content_layout)
        s.shapes.title.text = "Budget"
        _fill_bullets(s.placeholders[1].text_frame, ["Headcount unchanged", "Tooling plus five"])
        _move_slide(prs, 6, 2)

    if variant == "reorder":
        _swap_slides(prs, 1, 3)

    _set_core_props(prs, "pptctl diff fixture")
    prs.save(str(path))
    _normalize_zip(path)


def _fill_bullets(text_frame, lines: list[str]) -> None:
    """One paragraph per line, replacing the placeholder's empty paragraph."""
    text_frame.text = lines[0]
    for line in lines[1:]:
        text_frame.add_paragraph().text = line


def _move_slide(prs: Presentation, src: int, dst: int) -> None:
    """Move the slide at 0-based position src to position dst by reordering
    the p:sldIdLst entries; slide parts keep their names."""
    lst = prs.slides._sldIdLst
    el = list(lst)[src]
    lst.remove(el)
    lst.insert(dst, el)


def _swap_slides(prs: Presentation, i: int, j: int) -> None:
    """Swap the 0-based positions i < j in the p:sldIdLst."""
    lst = prs.slides._sldIdLst
    ids = list(lst)
    lst.remove(ids[i])
    lst.remove(ids[j])
    lst.insert(i, ids[j])
    lst.insert(j, ids[i])


def build_bloat_deck(path: Path) -> None:
    """3 slides for the img-bloat rule.
    1. 1600x1200 solid PNG (~7 KiB) shown 1 in wide: ~1600 DPI but under the
       64 KiB stored-size floor, so lint must NOT flag it by default.
    2. 200x150 noise PNG (~90 KiB) shown 0.5 in wide: ~400 DPI, flagged.
    3. 64x48 PNG shown 3 in tall: ~21 DPI, clean."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    s1 = prs.slides.add_slide(blank)
    s1.shapes.add_picture(
        io.BytesIO(_png(1600, 1200, (200, 40, 40))), Inches(1), Inches(1), width=Inches(1)
    )
    s2 = prs.slides.add_slide(blank)
    s2.shapes.add_picture(
        io.BytesIO(_noise_png(200, 150, 7)), Inches(1), Inches(1), width=Inches(0.5)
    )
    s3 = prs.slides.add_slide(blank)
    s3.shapes.add_picture(
        io.BytesIO(_png(64, 48, (40, 40, 200))), Inches(1), Inches(1), height=Inches(3)
    )
    _set_core_props(prs, "pptctl image bloat fixture")
    prs.save(str(path))
    _normalize_zip(path)


def build_crop_deck(path: Path) -> None:
    """2 slides for the optimize crop pass (#104).
    1. 160x120 noise PNG shown 2 in wide, cropped 25 % left and right and
       50 % from the bottom: only the 80x60 region at (40, 0) is visible.
    2. 120x90 noise PNG shown twice (one media part): cropped 50 % from the
       left, and uncropped. Differing crops: the pass must leave it alone."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    s1 = prs.slides.add_slide(blank)
    pic = s1.shapes.add_picture(
        io.BytesIO(_noise_png(160, 120, 11)), Inches(1), Inches(1), width=Inches(2)
    )
    pic.crop_left = 0.25
    pic.crop_right = 0.25
    pic.crop_bottom = 0.5
    s2 = prs.slides.add_slide(blank)
    shared = _noise_png(120, 90, 12)
    half = s2.shapes.add_picture(io.BytesIO(shared), Inches(1), Inches(1), width=Inches(2))
    half.crop_left = 0.5
    s2.shapes.add_picture(io.BytesIO(shared), Inches(4), Inches(1), width=Inches(2))
    _set_core_props(prs, "pptctl crop fixture")
    prs.save(str(path))
    _normalize_zip(path)


def _wave_jpeg(width: int, height: int, seed: int) -> bytes:
    """A photo-like RGB JPEG: smooth colour ramps over a blocky texture, so it
    compresses like a photo rather than a flat fill. Encoded by Pillow, which is
    python-pptx's own dependency (pinned above for stable bytes)."""
    from PIL import Image

    px = bytearray()
    for y in range(height):
        for x in range(width):
            px += bytes(
                (
                    (x * 255 // width + seed) & 0xFF,
                    (y * 255 // height) & 0xFF,
                    ((x // 24 + y // 16) * 37 + seed) & 0xFF,
                )
            )
    out = io.BytesIO()
    Image.frombytes("RGB", (width, height), bytes(px)).save(
        out, "JPEG", quality=85, subsampling=2, optimize=False, progressive=False
    )
    return out.getvalue()


def _alpha_png(width: int, height: int) -> bytes:
    """An RGBA PNG whose colour and alpha ramp across the image. Rows use the
    Sub filter, so it stays a few KiB however large it is."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    rows = bytearray()
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes(
                (x * 255 // width, y * 255 // height, (x + y) * 255 // (width + height), 255 - x * 255 // width)
            )
        # Sub filter: each byte minus the one 4 bytes (one pixel) to its left.
        rows.append(1)
        rows += bytes((row[i] - (row[i - 4] if i >= 4 else 0)) & 0xFF for i in range(len(row)))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )


def build_optimize_deck(path: Path) -> None:
    """3 slides for the optimize goldens (#105); inject_optimize_extras adds the rest.
    1. 1200x900 JPEG shown 2 in wide: the images pass downsamples it to 384x288.
    2. 800x600 PNG with alpha shown 1 in wide: downsampled to 192x144, alpha kept.
    3. 800x600 JPEG shown 3 in wide, cropped 25 % left and right, under a text
       box: the crop pass keeps 408x600 losslessly from x=192 (the visible
       region starts at 200, between two 16 px blocks; srcRect keeps the 8)."""
    prs = Presentation()
    blank = prs.slide_layouts[6]
    s1 = prs.slides.add_slide(blank)
    s1.shapes.add_picture(
        io.BytesIO(_wave_jpeg(1200, 900, 0)), Inches(1), Inches(1), width=Inches(2)
    )
    s2 = prs.slides.add_slide(blank)
    s2.shapes.add_picture(io.BytesIO(_alpha_png(800, 600)), Inches(1), Inches(1), width=Inches(1))
    s3 = prs.slides.add_slide(blank)
    s3.shapes.add_textbox(Inches(1), Inches(0.3), Inches(6), Inches(0.6)).text_frame.text = (
        "Cropped photo"
    )
    pic = s3.shapes.add_picture(
        io.BytesIO(_wave_jpeg(800, 600, 90)), Inches(1), Inches(1), width=Inches(3)
    )
    pic.crop_left = 0.25
    pic.crop_right = 0.25
    _set_core_props(prs, "pptctl optimize fixture")
    prs.save(str(path))
    _normalize_zip(path)


def inject_optimize_extras(path: Path) -> None:
    """Add to optimize_deck.pptx what python-pptx can't make: an orphan media
    part nothing references, and an embedded font (ppt/fonts/font1.fntdata,
    its presentation relationship, p:embeddedFontLst, embedTrueTypeFonts and
    the fntdata Default). The font bytes are a fixed LCG stream, not a font:
    the fonts pass only removes them."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}

    parts["ppt/media/orphan.png"] = _png(64, 64, (1, 2, 3))

    state, font = 20251, bytearray()
    for _ in range(6000):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        font.append((state >> 16) & 0xFF)
    parts["ppt/fonts/font1.fntdata"] = bytes(font)

    rels = parts["ppt/_rels/presentation.xml.rels"].decode("utf-8")
    used = [int(m) for m in re.findall(r'Id="rId(\d+)"', rels)]
    rid = f"rId{max(used) + 1}"
    rels = rels.replace(
        "</Relationships>",
        f'<Relationship Id="{rid}" Type="{_REL_NS}/font" Target="fonts/font1.fntdata"/>'
        "</Relationships>",
    )
    parts["ppt/_rels/presentation.xml.rels"] = rels.encode("utf-8")

    pres = parts["ppt/presentation.xml"].decode("utf-8")
    pres = pres.replace("<p:presentation ", '<p:presentation embedTrueTypeFonts="1" ', 1)
    pres = re.sub(
        r"(<p:notesSz [^>]*/>)",
        r'\1<p:embeddedFontLst><p:embeddedFont><p:font typeface="Lato"/>'
        f'<p:regular r:id="{rid}"/></p:embeddedFont></p:embeddedFontLst>',
        pres,
        count=1,
    )
    parts["ppt/presentation.xml"] = pres.encode("utf-8")

    ct = parts["[Content_Types].xml"].decode("utf-8")
    ct = re.sub(
        r"(<Types [^>]*>)",
        r'\1<Default Extension="fntdata" ContentType="application/x-fontdata"/>',
        ct,
        count=1,
    )
    parts["[Content_Types].xml"] = ct.encode("utf-8")

    _write_zip(path, parts)


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

    # 7. img-bloat: 200x150 noise PNG (~90 KiB, above the stored-size floor)
    #    shown 0.5 in wide (about 400 DPI effective).
    s = prs.slides.add_slide(blank)
    s.shapes.add_picture(
        io.BytesIO(_noise_png(200, 150, 7)), Inches(1), Inches(1), width=Inches(0.5)
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


def build_integrity_deck(path: Path) -> None:
    """Base for integrity_problems.pptx: a picture slide with speaker notes,
    then two chart slides (each chart has its own .rels for the embedded
    workbook). Ordinary python-pptx output; inject_integrity_problems() breaks
    the package afterwards."""
    from pptx.chart.data import ChartData
    from pptx.enum.chart import XL_CHART_TYPE

    prs = Presentation()
    blank = prs.slide_layouts[6]

    s = prs.slides.add_slide(blank)
    s.shapes.add_picture(
        io.BytesIO(_png(64, 48, (200, 40, 40))), Inches(1), Inches(1), height=Inches(3)
    )
    s.notes_slide.notes_text_frame.text = "Notes owned by slide 1"

    for chart_type, values in ((XL_CHART_TYPE.BAR_CLUSTERED, (1, 2, 3)), (XL_CHART_TYPE.PIE, (3, 2, 1))):
        s = prs.slides.add_slide(blank)
        cd = ChartData()
        cd.categories = ["A", "B", "C"]
        cd.add_series("S", values)
        s.shapes.add_chart(chart_type, Inches(1), Inches(1), Inches(6), Inches(4), cd)

    _set_core_props(prs, "pptctl integrity problems fixture")
    prs.save(str(path))
    _normalize_zip(path)


def _add_rel(rels: bytes, rel_type: str, target: str) -> tuple[bytes, str]:
    """Append a Relationship with a fresh rId to a .rels part; returns the new
    bytes and the rId."""
    text = rels.decode("utf-8")
    used = [int(m) for m in re.findall(r'Id="rId(\d+)"', text)]
    rid = f"rId{(max(used) + 1) if used else 1}"
    text = text.replace(
        "</Relationships>",
        f'<Relationship Id="{rid}" Type="{rel_type}" Target="{target}"/></Relationships>',
    )
    return text.encode("utf-8"), rid


def inject_integrity_problems(path: Path) -> None:
    """Break the package at the ZIP level, one defect per integrity rule
    (python-pptx can't write broken packages). Each defect trips only its own
    rule:
      dangling-rel      slide 1 rels -> ../media/missing.png (absent)
      broken-rid        slide 1's picture r:embed -> rId99 (not in its rels)
      undeclared-ctype  ppt/media/extra.webp, reached from slide 1, no webp
                        Default; and a stale Override for /ppt/slides/slide99.xml
      orphan-part       ppt/media/orphan.png, nothing references it
      shared-part       slide 2 also targets slide 1's notesSlide; both charts
                        target one chartStyle part"""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}

    slide1_rels = "ppt/slides/_rels/slide1.xml.rels"
    parts[slide1_rels], _ = _add_rel(parts[slide1_rels], f"{_REL_NS}/image", "../media/missing.png")

    slide1 = parts["ppt/slides/slide1.xml"].decode("utf-8")
    slide1 = re.sub(r'r:embed="rId\d+"', 'r:embed="rId99"', slide1, count=1)
    parts["ppt/slides/slide1.xml"] = slide1.encode("utf-8")

    parts["ppt/media/extra.webp"] = b"RIFF\x00\x00\x00\x00WEBP"
    parts[slide1_rels], _ = _add_rel(parts[slide1_rels], f"{_REL_NS}/image", "../media/extra.webp")

    parts["ppt/media/orphan.png"] = _png(16, 16, (9, 9, 9))

    notes = re.search(r'Target="../notesSlides/(notesSlide\d+\.xml)"', parts[slide1_rels].decode())
    assert notes, "slide 1 has no notes slide"
    slide2_rels = "ppt/slides/_rels/slide2.xml.rels"
    parts[slide2_rels], _ = _add_rel(
        parts[slide2_rels], f"{_REL_NS}/notesSlide", f"../notesSlides/{notes.group(1)}"
    )

    style_rel = "http://schemas.microsoft.com/office/2011/relationships/chartStyle"
    parts["ppt/charts/style1.xml"] = (
        b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        b'<cs:chartStyle xmlns:cs="http://schemas.microsoft.com/office/drawing/2012/chartStyle" '
        b'id="201"/>'
    )
    chart_rels = sorted(n for n in parts if re.fullmatch(r"ppt/charts/_rels/chart\d+\.xml\.rels", n))
    assert len(chart_rels) == 2, chart_rels
    for rels in chart_rels:
        parts[rels], _ = _add_rel(parts[rels], style_rel, "style1.xml")

    ct = parts["[Content_Types].xml"].decode("utf-8")
    ct = ct.replace(
        "</Types>",
        '<Override PartName="/ppt/charts/style1.xml" '
        'ContentType="application/vnd.ms-office.chartstyle+xml"/>'
        '<Override PartName="/ppt/slides/slide99.xml" '
        'ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
        "</Types>",
    )
    parts["[Content_Types].xml"] = ct.encode("utf-8")

    _write_zip(path, parts)


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


# The logo merge_a and merge_c share: identical bytes in both packages.
_MERGE_LOGO = (96, 48, (20, 90, 160))


def build_merge_deck_a(path: Path) -> None:
    """merge_a.pptx: a title slide, then two slides showing the shared logo."""
    prs = Presentation()
    s = prs.slides.add_slide(prs.slide_layouts[0])  # Title Slide
    s.shapes.title.text = "Deck A"
    s.placeholders[1].text_frame.text = "The first deck to merge"
    s = prs.slides.add_slide(prs.slide_layouts[1])  # Title and Content
    s.shapes.title.text = "A: bullets"
    _fill_bullets(s.placeholders[1].text_frame, ["Alpha", "Beta", "Gamma"])
    s.shapes.add_picture(io.BytesIO(_png(*_MERGE_LOGO)), Inches(8), Inches(0.2), height=Inches(0.5))
    s = prs.slides.add_slide(prs.slide_layouts[6])  # Blank
    s.shapes.add_textbox(Inches(1), Inches(0.3), Inches(6), Inches(0.6)).text_frame.text = (
        "A: picture"
    )
    s.shapes.add_picture(io.BytesIO(_png(*_MERGE_LOGO)), Inches(1), Inches(1), height=Inches(2))
    _set_core_props(prs, "pptctl merge fixture A")
    prs.save(str(path))
    _normalize_zip(path)


def build_merge_deck_b(path: Path) -> None:
    """merge_b.pptx before restyle_merge_theme: two slides with speaker notes,
    the second holding a clustered-bar chart."""
    from pptx.chart.data import ChartData
    from pptx.enum.chart import XL_CHART_TYPE

    prs = Presentation()
    s = prs.slides.add_slide(prs.slide_layouts[1])  # Title and Content
    s.shapes.title.text = "B: bullets"
    _fill_bullets(s.placeholders[1].text_frame, ["One", "Two"])
    s.notes_slide.notes_text_frame.text = "Notes for B, slide 1"
    s = prs.slides.add_slide(prs.slide_layouts[5])  # Title Only
    s.shapes.title.text = "B: chart"
    cd = ChartData()
    cd.categories = ["Q1", "Q2", "Q3"]
    cd.add_series("Sales", (4, 7, 5))
    s.shapes.add_chart(
        XL_CHART_TYPE.COLUMN_CLUSTERED, Inches(1), Inches(1.5), Inches(8), Inches(5), cd
    )
    s.notes_slide.notes_text_frame.text = "Notes for B, slide 2"
    _set_core_props(prs, "pptctl merge fixture B")
    prs.save(str(path))
    _normalize_zip(path)


def restyle_merge_theme(path: Path) -> None:
    """Give merge_b.pptx's theme its own name, colors and fonts, so a merge can
    tell its slides' formatting from the default template's."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}
    theme = parts["ppt/theme/theme1.xml"].decode("utf-8")
    theme = theme.replace('name="Office Theme"', 'name="pptctl Contrast"', 1)
    theme = theme.replace('<a:clrScheme name="Office"', '<a:clrScheme name="pptctl Contrast"', 1)
    theme = theme.replace('<a:fontScheme name="Office"', '<a:fontScheme name="pptctl Contrast"', 1)
    accents = {
        "accent1": "C0392B",
        "accent2": "D35400",
        "accent3": "16A085",
        "accent4": "8E44AD",
        "accent5": "2C3E50",
        "accent6": "F39C12",
    }
    for slot, rgb in accents.items():
        theme = re.sub(
            rf"<a:{slot}><a:srgbClr val=\"[0-9A-F]{{6}}\"/></a:{slot}>",
            f'<a:{slot}><a:srgbClr val="{rgb}"/></a:{slot}>',
            theme,
        )
    # Major (headings) then minor (body) Latin typefaces.
    latin = iter(("Georgia", "Verdana"))
    theme = re.sub(
        r'<a:latin typeface="Calibri"', lambda _: f'<a:latin typeface="{next(latin)}"', theme, count=2
    )
    parts["ppt/theme/theme1.xml"] = theme.encode("utf-8")
    _write_zip(path, parts)


def add_merge_sections(path: Path) -> None:
    """Split merge_b.pptx into two PowerPoint 2010 sections, "Intro" (slide 1)
    and "Data" (slide 2); python-pptx has no API for sections."""
    with zipfile.ZipFile(path) as zf:
        parts = {n: zf.read(n) for n in zf.namelist()}
    pres = parts["ppt/presentation.xml"].decode("utf-8")
    ids = re.findall(r'<p:sldId id="(\d+)"', pres)
    sections = "".join(
        f'<p14:section name="{name}" id="{guid}"><p14:sldIdLst><p14:sldId id="{sid}"/>'
        "</p14:sldIdLst></p14:section>"
        for name, guid, sid in (
            ("Intro", "{6F1A3C52-0B7E-4D8A-9C21-5E4B7A2D9F10}", ids[0]),
            ("Data", "{A93E7D04-2C5B-4F61-8E3A-1B7C9D0E2F43}", ids[1]),
        )
    )
    ext = (
        '<p:extLst><p:ext uri="{521415D9-36F7-43E2-AB2F-B90AF26B5E84}">'
        '<p14:sectionLst xmlns:p14="http://schemas.microsoft.com/office/powerpoint/2010/main">'
        f"{sections}</p14:sectionLst></p:ext></p:extLst>"
    )
    parts["ppt/presentation.xml"] = pres.replace("</p:presentation>", ext + "</p:presentation>").encode(
        "utf-8"
    )
    _write_zip(path, parts)


def build_merge_deck_c(path: Path) -> None:
    """merge_c.pptx: two slides on merge_a's template, both showing the logo.
    Slide 1 has an external hyperlink; slide 2's logo links back to slide 1
    (an internal slide-to-slide relationship)."""
    prs = Presentation()
    first = prs.slides.add_slide(prs.slide_layouts[1])  # Title and Content
    first.shapes.title.text = "C: bullets"
    _fill_bullets(first.placeholders[1].text_frame, ["Delta", "Epsilon"])
    first.placeholders[1].text_frame.paragraphs[0].runs[0].hyperlink.address = (
        "https://example.com/pptctl"
    )
    first.shapes.add_picture(
        io.BytesIO(_png(*_MERGE_LOGO)), Inches(8), Inches(0.2), height=Inches(0.5)
    )
    s = prs.slides.add_slide(prs.slide_layouts[5])  # Title Only
    s.shapes.title.text = "C: closing"
    logo = s.shapes.add_picture(
        io.BytesIO(_png(*_MERGE_LOGO)), Inches(8), Inches(0.2), height=Inches(0.5)
    )
    logo.click_action.target_slide = first
    _set_core_props(prs, "pptctl merge fixture C")
    prs.save(str(path))
    _normalize_zip(path)


_FIXTURES: list[tuple[str, list[object]]] = [
    ("images_deck.pptx", [build_images_deck]),
    ("images_deck_v2.pptx", [build_images_deck_v2, rename_media_parts]),
    ("bloat_deck.pptx", [build_bloat_deck]),
    ("crop_deck.pptx", [build_crop_deck]),
    ("optimize_deck.pptx", [build_optimize_deck, inject_second_master, inject_optimize_extras]),
    ("lint_problems.pptx", [build_lint_problems_deck]),
    ("lint_clean.pptx", [build_lint_clean_deck, strip_printer_settings]),
    ("notes_and_masters.pptx", [build_notes_deck_base, inject_second_master]),
    ("tables_deck.pptx", [build_tables_deck]),
    ("charts_deck.pptx", [build_charts_deck]),
    ("diff_base.pptx", [build_diff_deck]),
    *[
        (f"diff_{v}.pptx", [functools.partial(build_diff_deck, variant=v)])
        for v in _DIFF_VARIANTS
    ],
    ("charts_deck_v2.pptx", [build_charts_deck_v2]),
    ("integrity_problems.pptx", [build_integrity_deck, inject_integrity_problems]),
    ("merge_a.pptx", [build_merge_deck_a]),
    ("merge_b.pptx", [build_merge_deck_b, restyle_merge_theme, add_merge_sections]),
    ("merge_c.pptx", [build_merge_deck_c]),
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
