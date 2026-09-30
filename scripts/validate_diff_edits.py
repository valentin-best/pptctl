#!/usr/bin/env -S uv run --python 3.12 --with python-pptx
# Known-edit generator and scorer for the Phase 4 `diff` validation (#77).
#
#   validate_diff_edits.py edit  DECK.pptx OUT_DIR   -> OUT_DIR/<name>.resaved.pptx,
#                                                       OUT_DIR/<name>.edited.pptx,
#                                                       OUT_DIR/<name>.expected.json
#   validate_diff_edits.py score EXPECTED.json REPORT.json -> prints a scorecard line,
#                                                             exit 0 if every edit found
#
# `edit` applies three known edits with python-pptx and records what to
# expect: a text edit on one slide, a shape move on another, and the deletion
# of the last slide. `resaved` is the deck re-serialized with no edits at all,
# to measure the round-trip noise floor separately from the edits.
#
# Dev-only; needs python-pptx via uv (see CLAUDE.md). Never run by CMake.

import json
import sys
from pathlib import Path

from pptx import Presentation
from pptx.util import Emu

MOVE_EMU = 228600  # 0.25 in


def slide_shapes(slide):
    for sh in slide.shapes:
        yield sh
        if sh.shape_type == 6:  # GROUP
            yield from sh.shapes


def pick_text_target(prs):
    """First (slide_idx, shape, paragraph, run) whose run has >= 2 words."""
    for i, slide in enumerate(prs.slides, 1):
        for sh in slide_shapes(slide):
            if not sh.has_text_frame:
                continue
            for p in sh.text_frame.paragraphs:
                for r in p.runs:
                    if len(r.text.split()) >= 2:
                        return i, sh, p, r
    return None


def pick_move_target(prs, avoid_slide):
    """First positioned shape on a slide other than avoid_slide."""
    for i, slide in enumerate(prs.slides, 1):
        if i == avoid_slide:
            continue
        for sh in slide.shapes:
            if sh.left is not None and sh.width is not None and sh.left > MOVE_EMU:
                return i, sh
    return None


def delete_last_slide(prs):
    sld_ids = prs.slides._sldIdLst
    entries = list(sld_ids)
    last = entries[-1]
    prs.part.drop_rel(last.rId)
    sld_ids.remove(last)
    return len(entries)


def cmd_edit(deck, out_dir):
    deck = Path(deck)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = deck.stem

    # 1. Pure re-save: the noise floor.
    Presentation(str(deck)).save(str(out_dir / f"{name}.resaved.pptx"))

    # 2. Known edits.
    prs = Presentation(str(deck))
    expected = {"deck": deck.name, "slides": len(prs.slides), "edits": []}

    t = pick_text_target(prs)
    if t:
        i, sh, p, r = t
        old = r.text
        r.text = old + " EDITED"
        expected["edits"].append(
            {"kind": "text", "slide": i, "shape": sh.name, "old": old, "new": r.text}
        )

    m = pick_move_target(prs, t[0] if t else 0)
    if m:
        i, sh = m
        sh.left = Emu(sh.left - MOVE_EMU)
        expected["edits"].append({"kind": "move", "slide": i, "shape": sh.name, "dx_emu": -MOVE_EMU})

    # Only remove a slide that carries none of the other edits, so every edit
    # stays observable; a deck too small for that just skips this edit.
    used = {e["slide"] for e in expected["edits"]}
    if len(prs.slides) not in used and len(prs.slides) >= 2:
        removed = delete_last_slide(prs)
        expected["edits"].append({"kind": "remove", "slide": removed})

    prs.save(str(out_dir / f"{name}.edited.pptx"))
    (out_dir / f"{name}.expected.json").write_text(json.dumps(expected, indent=2))
    print(json.dumps(expected))


def find_slide(report, a_index):
    for s in report["slides"]:
        if s["a_index"] == a_index:
            return s
    return None


def cmd_score(expected_path, report_path):
    exp = json.loads(Path(expected_path).read_text())
    rep = json.loads(Path(report_path).read_text())
    found = []
    missing = []
    touched = set()
    for e in exp["edits"]:
        s = find_slide(rep, e["slide"])
        touched.add(e["slide"])
        ok = False
        if e["kind"] == "text" and s and s["status"] == "changed":
            ok = any(
                t["kind"] in ("changed", "inserted") and t["new_text"] and e["new"] in t["new_text"]
                for t in s["changes"]["text"]
            )
        elif e["kind"] == "move" and s and s["status"] == "changed":
            # A placeholder that inherited its position has no delta: the op
            # carries only the new explicit geometry, which is still a hit.
            ok = any(
                c["op"] == "moved"
                and c["name"] == e["shape"]
                and (c["delta"] is None or c["delta"]["x_emu"] == e["dx_emu"])
                for c in s["changes"]["shapes"]
            )
        elif e["kind"] == "remove":
            ok = bool(s) and s["status"] == "removed"
        (found if ok else missing).append(e)

    # Spurious: any slide outside the edited ones that is not plainly unchanged,
    # plus ops on the edited slides beyond the one we made.
    spurious_slides = [
        s for s in rep["slides"]
        if s["a_index"] not in touched and (s["status"] != "unchanged" or s["moved"])
    ]
    extra_ops = 0
    for e in exp["edits"]:
        s = find_slide(rep, e["slide"])
        if not s or s["status"] != "changed":
            continue
        n = sum(len(v) for v in s["changes"].values())
        extra_ops += max(0, n - 1)

    print(
        f"{exp['deck']:40s} edits {len(found)}/{len(exp['edits'])} found"
        f"  spurious slides {len(spurious_slides)}  extra ops {extra_ops}"
    )
    for e in missing:
        print(f"    MISSING {e}")
    for s in spurious_slides[:5]:
        print(f"    SPURIOUS slide a={s['a_index']} b={s['b_index']} {s['status']} moved={s['moved']}")
    return 0 if not missing else 1


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "edit":
        cmd_edit(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "score":
        sys.exit(cmd_score(sys.argv[2], sys.argv[3]))
    else:
        print(__doc__ or "usage: see header", file=sys.stderr)
        sys.exit(2)
