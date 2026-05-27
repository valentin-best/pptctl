#!/usr/bin/env python3
"""Deterministically (re)generate the synthetic .pptx test fixtures.

These fixtures are committed binaries; this script is the source of truth for
how they were built, so reviewers can audit their contents and we can regenerate
them if requirements change. It is dev-only tooling — CMake never invokes it and
the C++ build has no Python dependency.

Run it with `uv` (no install needed):

    uv run --python 3.12 --with python-pptx tests/fixtures/generate_fixtures.py

Generates, next to this script:
  - images_deck.pptx        12 slides, each with a distinct embedded PNG.
  - notes_and_masters.pptx  4 slides with speaker notes, plus a 2nd slide master.

python-pptx has no API for a second slide master, so notes_and_masters.pptx is
built normally and then post-processed at the package (ZIP/parts) level to inject
a second master with its own layout and theme. Counts asserted by the C++ tests:
12 slides / 12 images, and >= 2 masters + a notesSlide part, respectively.
"""

from __future__ import annotations

import datetime
import io
import re
import struct
import zlib
from pathlib import Path

from pptx import Presentation
from pptx.util import Emu, Inches

HERE = Path(__file__).resolve().parent

# Fixed metadata so regenerated fixtures don't churn on author/timestamps.
_FIXED_DT = datetime.datetime(2025, 1, 1, 0, 0, 0)
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


def _next_index(names: list[str], pattern: str) -> int:
    """Highest numeric suffix matching `pattern` (one capture group), plus 1."""
    rx = re.compile(pattern)
    nums = [int(m.group(1)) for n in names if (m := rx.fullmatch(n))]
    return (max(nums) + 1) if nums else 1


def inject_second_master(path: Path) -> None:
    """Add a second slide master (its own layout + theme) by duplicating the
    first at the package level. python-pptx exposes no API for this."""
    import zipfile

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

    # Rewrite the archive with a fixed timestamp for reproducible output.
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in parts.items():
            info = zipfile.ZipInfo(name, date_time=(2025, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data)


def main() -> None:
    images = HERE / "images_deck.pptx"
    notes = HERE / "notes_and_masters.pptx"
    build_images_deck(images)
    build_notes_deck_base(notes)
    inject_second_master(notes)
    print(f"wrote {images.name} and {notes.name} to {HERE}")


if __name__ == "__main__":
    main()
