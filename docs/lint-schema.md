# `pptctl lint` JSON Output Schema

**Current version: 1**

`pptctl lint --json FILE` emits a single JSON object to stdout. This document
is the authoritative spec for that output. Downstream tools (CI gates, editor
integrations, golden tests) should key off `schema_version` to guard against
breaking changes.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or meaning, changing the ordering guarantee) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new rule ids, new severities) do
  not bump the version; consumers should ignore unknown fields and unknown
  rule ids.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `file` | `string` | Path passed to the command. |
| `diagnostics` | `array<Diagnostic>` | Every finding, in the order described below. Empty when the deck is clean. |
| `summary` | `Summary` | Counts by severity. |

### Ordering

Deck-level diagnostics (those with `slide: null`) come first, followed by
slide-level diagnostics in ascending slide order. Within one slide, the order
is the rule evaluation order and is stable across runs, so the document is
byte-for-byte reproducible for a given deck and `pptctl` version.

---

## Diagnostic

| Field | Type | Description |
|---|---|---|
| `rule` | `string` | Stable rule id (see below). |
| `severity` | `"warning"` \| `"error"` | Severity. Today every rule emits `warning`. |
| `slide` | `integer` \| `null` | 1-based slide index, or `null` for a deck-level finding (fonts, content types). |
| `shape` | `string` \| `null` | Name of the shape the finding is attached to (`p:cNvPr@name`), or `null` when the finding is not shape-specific. |
| `message` | `string` | Human-readable description. Wording may change between releases; key off `rule`, not `message`. |

### Rule ids

| `rule` | Meaning |
|---|---|
| `overflow` | Shape geometry extends past the slide boundary. |
| `tiny-text` | A text run is below the minimum font size (default 12 pt, `--min-font-size`). |
| `alt-text` | A picture has no alt text and is not marked decorative. |
| `font-risk` | A font is referenced but neither embedded nor on the safe list. |
| `empty-ph` | A content placeholder contains no text. |
| `ctype` | A part has a suspect content type, or none declared at all. |
| `img-bloat` | An image is stored at far higher resolution than it is displayed (default: above 300 DPI effective and at least 64 KiB stored; `--max-image-dpi`, `--min-image-bytes`). |

---

## Summary

| Field | Type | Description |
|---|---|---|
| `warnings` | `integer` | Number of diagnostics with `severity: "warning"`. |
| `errors` | `integer` | Number of diagnostics with `severity: "error"`. |

---

## Exit codes

Exit codes are the same with and without `--json`.

| Code | Meaning |
|---|---|
| `0` | Deck was read. Without `--strict`, this is returned even when diagnostics were emitted. |
| `1` | The file could not be opened or is not a valid `.pptx`. Nothing is written to stdout. |
| `2` | `--strict` was given and at least one diagnostic was emitted. |

---

## Example

```json
{
  "diagnostics": [
    {
      "message": "part \"ppt/printerSettings/printerSettings1.bin\" has suspect content type \"application/vnd.openxmlformats-officedocument.presentationml.printerSettings\"",
      "rule": "ctype",
      "severity": "warning",
      "shape": null,
      "slide": null
    },
    {
      "message": "placeholder \"Subtitle 2\" (subTitle, idx 1) is empty",
      "rule": "empty-ph",
      "severity": "warning",
      "shape": "Subtitle 2",
      "slide": 1
    }
  ],
  "file": "deck.pptx",
  "schema_version": 1,
  "summary": {
    "errors": 0,
    "warnings": 2
  }
}
```

Keys are emitted in sorted order (as shown) because the serializer uses an
ordered map; consumers should not depend on key order.
