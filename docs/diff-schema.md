# `pptctl diff` JSON Output Schema

**Current version: 1**

`pptctl diff --format json A B` emits a single JSON object to stdout: the full
slide mapping between the two decks, including unchanged slides, so a tool can
reconstruct where every slide went. This document is the authoritative spec
for that output. Downstream tools (review bots, CI gates, golden tests) should
key off `schema_version` to guard against breaking changes.

The unified (`--format unified`, the default) and HTML (`--format html`)
renderers are views of the same report; only the JSON shape is versioned.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or meaning, changing the ordering guarantee) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new op kinds inside `changes`)
  do not bump the version; consumers should ignore unknown fields and unknown
  op kinds.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `a` | `string` | Path of the old deck, as passed to the command. |
| `b` | `string` | Path of the new deck, as passed to the command. |
| `slides` | `array<Slide>` | One entry per slide of A ∪ B, in the order described below. |
| `summary` | `Summary` | Counts by status. |

### Ordering

Entries follow the order of deck B. A slide that exists only in A (`removed`)
is slotted at the position it had in A, so reading top to bottom walks the
new deck while still showing where old slides disappeared. The order is
stable across runs: the document is byte-for-byte reproducible for a given
pair of decks and `pptctl` version.

---

## Slide

| Field | Type | Description |
|---|---|---|
| `status` | `"unchanged"` \| `"changed"` \| `"added"` \| `"removed"` | How the slide was classified (see below). |
| `a_index` | `integer` \| `null` | 1-based position in A. `null` when `status` is `added`. |
| `b_index` | `integer` \| `null` | 1-based position in B. `null` when `status` is `removed`. |
| `moved` | `boolean` | `true` for a matched slide whose order relative to its peers changed. Always `false` for `added` / `removed`. |
| `similarity` | `number` | Token similarity in `[0, 1]`: `1` for an exact fingerprint match, the best-match score for `changed`, `0` for `added` / `removed`. |
| `changes` | `Changes` | Nested ops. Non-empty only when `status` is `changed`. |

### Status

| `status` | Meaning |
|---|---|
| `unchanged` | Same content fingerprint in both decks. May still be `moved`. |
| `changed` | No exact match, but paired with its most similar counterpart above `--match-threshold` (default `0.5`). `changes` says what differs. |
| `added` | Present in B only. |
| `removed` | Present in A only. |

---

## Changes

| Field | Type | Description |
|---|---|---|
| `text` | `array<TextChange>` | Text-level ops within matched shapes and the notes. |
| `shapes` | `array<ShapeChange>` | Structural shape ops: added, removed, moved, resized, restyled. |
| `pictures` | `array<PictureChange>` | Picture media ops: added, removed, replaced, re-linked, alt-text. |

All three arrays are always present (empty for slides that are not
`changed`). Their per-op keys are defined by the issues that populate them —
`text` by #70, `shapes` by #72, `pictures` by #73 — and are documented here
as each lands. Until then every array is empty. Adding a new op kind is an
additive change.

---

## Summary

| Field | Type | Description |
|---|---|---|
| `unchanged` | `integer` | Number of slides with `status: "unchanged"`. |
| `changed` | `integer` | Number of slides with `status: "changed"`. |
| `added` | `integer` | Number of slides with `status: "added"`. |
| `removed` | `integer` | Number of slides with `status: "removed"`. |
| `moved` | `integer` | Number of slides with `moved: true` (counted across `unchanged` and `changed`). |

---

## Exit codes

Exit codes follow `git diff` and are the same in every format and under
`--quiet` (which suppresses stdout entirely).

| Code | Meaning |
|---|---|
| `0` | The decks are identical: every slide is `unchanged` and none is `moved`. |
| `1` | Differences were found. |
| `2` | Error: a file could not be opened or is not a valid `.pptx`, or `--format` is unknown. Nothing is written to stdout. |

---

## Example

`minimal.pptx` (one slide) against `three_slides.pptx` (three slides):

```json
{
  "a": "minimal.pptx",
  "b": "three_slides.pptx",
  "schema_version": 1,
  "slides": [
    {
      "a_index": 1,
      "b_index": 1,
      "changes": {
        "pictures": [],
        "shapes": [],
        "text": []
      },
      "moved": false,
      "similarity": 1.0,
      "status": "unchanged"
    },
    {
      "a_index": null,
      "b_index": 2,
      "changes": {
        "pictures": [],
        "shapes": [],
        "text": []
      },
      "moved": false,
      "similarity": 0.0,
      "status": "added"
    },
    {
      "a_index": null,
      "b_index": 3,
      "changes": {
        "pictures": [],
        "shapes": [],
        "text": []
      },
      "moved": false,
      "similarity": 0.0,
      "status": "added"
    }
  ],
  "summary": {
    "added": 2,
    "changed": 0,
    "moved": 0,
    "removed": 0,
    "unchanged": 1
  }
}
```

Keys are emitted in sorted order (as shown) because the serializer uses an
ordered map; consumers should not depend on key order.
