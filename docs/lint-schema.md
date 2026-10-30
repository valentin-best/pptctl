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
| `severity` | `"warning"` \| `"error"` | Severity. Content rules emit `warning`; package-integrity rules may emit `error`. |
| `slide` | `integer` \| `null` | 1-based slide index, or `null` for a deck-level finding (fonts, content types, most integrity findings). |
| `shape` | `string` \| `null` | Name of the shape the finding is attached to (`p:cNvPr@name`), or `null` when the finding is not shape-specific. |
| `part` | `string` \| `null` | Package part the finding is about (e.g. `ppt/slides/_rels/slide3.xml.rels`), or `null`. Set by the package-integrity rules. Added in v0.6.0 without a version bump (additive). |
| `message` | `string` | Human-readable description. Wording may change between releases; key off `rule`, not `message`. |

### Rule ids

| `rule` | Meaning |
|---|---|
| `overflow` | Shape geometry extends past the slide boundary. |
| `tiny-text` | A text run is below the minimum font size (default 12 pt, `--min-font-size`). |
| `alt-text` | A picture has no alt text and is not marked decorative. |
| `font-risk` | A font is referenced but neither embedded nor bundled with the target platform (`--target all\|windows\|macos`; Office-bundled fonts always count as safe). The message says where it substitutes. |
| `empty-ph` | A content placeholder contains no text. |
| `ctype` | A part has a suspect content type, or none declared at all. |
| `img-bloat` | An image is stored at far higher resolution than it is displayed (default: above 300 DPI effective and at least 64 KiB stored; `--max-image-dpi`, `--min-image-bytes`). |

#### Package-integrity rules

These check the package itself (parts, relationships, content types), not
slide content. `--only integrity` selects all of them; it is how `merge`
output and decks written by other tools are verified.

| `rule` | Meaning |
|---|---|
| `dangling-rel` | An internal relationship targets a part that doesn't exist. |
| `broken-rid` | An `r:embed` / `r:id` / `r:link` names a relationship id the part's `.rels` doesn't define. |
| `undeclared-ctype` | A part has neither a `Default` nor an `Override` in `[Content_Types].xml`. |
| `orphan-part` | No relationship reaches a part. |
| `shared-part` | A part that must have a single owner (notes, comments, tags, charts and their style parts) is targeted from several. |

### Selecting rules

`--only` restricts the run to the given rule ids and group aliases
(comma-separated or repeated; `integrity` is the only group). Unselected
rules produce no diagnostics. An unknown id is a usage error.

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
| `1` | The file could not be opened as a ZIP (nothing is written to stdout), or its `presentation.xml` could not be loaded. In the second case the integrity rules have still run and, when any is selected, their report is written to stdout; the content rules are skipped. With only integrity rules selected (`--only integrity`), `presentation.xml` is never loaded. |
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
