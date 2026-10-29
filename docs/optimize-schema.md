# `pptctl optimize` JSON Output Schema

**Current version: 1**

`pptctl optimize --json FILE (-o OUT | --in-place | --dry-run)` emits a single
JSON object to stdout describing what each pass did. This document is the
authoritative spec for that output. Downstream tools (CI size budgets, build
pipelines, golden tests) should key off `schema_version` to guard against
breaking changes.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or meaning, changing the ordering guarantee) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new pass ids, new action kinds)
  do not bump the version; consumers should ignore unknown fields, pass ids
  and action kinds.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `input` | `string` | Path passed to the command. |
| `output` | `string` \| `null` | Where the result was written: the `-o` path, the input path for `--in-place`, `null` for `--dry-run`. |
| `dry_run` | `boolean` | `true` when nothing was written. |
| `input_bytes` | `integer` | Size of the input file, in bytes. |
| `output_bytes` | `integer` | Size of the resulting file, in bytes. Computed on a dry run too (the result is built in a temporary directory and measured). |
| `passes` | `array<Pass>` | One entry per **enabled** pass, in run order. A pass turned off by its opt-out flag is absent. |

The saving is `input_bytes - output_bytes`. Per-action sizes (below) are
uncompressed part sizes, so they don't add up to the file-size change exactly:
the ZIP container compresses most parts.

---

## Pass

| Field | Type | Description |
|---|---|---|
| `id` | `string` | Stable pass id (see below). |
| `actions` | `array<Action>` | What the pass did, one entry per part. Empty when it had nothing to do. |

### Pass ids, in run order

| `id` | What it does | Opt-out |
|---|---|---|
| `layouts` | Removes slide layouts no slide uses, slide masters left with none, and whatever only they reached (a master's theme, media only a removed layout showed). A deck always keeps one master, and a master always keeps one layout. | `--no-layouts` |
| `fonts` | Removes embedded fonts (`p:embeddedFontLst` and the font parts) and turns off "embed fonts" so PowerPoint doesn't re-embed them on the next save. | `--keep-fonts` |
| `crop` | Cuts the pixels outside a picture's crop (`a:srcRect`) when every use of the image has the same crop, and rewrites the crop to match. PNGs are re-encoded losslessly; JPEGs are cropped losslessly in the DCT domain. | `--no-crop` |
| `images` | Downsamples PNG and JPEG images to their largest displayed size at the target density (`--max-scale`, default 2 × 96 DPI; or `--max-dpi`) and re-encodes them (JPEG at `--quality`, default 85; PNG losslessly). | `--no-images` |
| `orphans` | Removes media parts nothing references. | `--no-orphans` |

Layouts run first, so media only they used is gone before the orphans pass
looks; crop runs before images, so images downsample only the visible region.

---

## Action

| Field | Type | Description |
|---|---|---|
| `part` | `string` | Part name, e.g. `"ppt/media/image3.jpeg"`. |
| `kind` | `string` | What happened to the part (see below). |
| `detail` | `string` | Kind-specific detail; may be `""`. Wording may change between releases; key off `kind`. |
| `bytes_before` | `integer` | Uncompressed size of the part before the pass. |
| `bytes_after` | `integer` | Uncompressed size after the pass; `0` when removed; equal to `bytes_before` when skipped. |

### Action kinds

| `kind` | Emitted by | `detail` |
|---|---|---|
| `removed` | `layouts` | What the part was: `"layout"`, `"master"`, `"theme"`, `"media"`, or `"only used by removed layouts"`. |
| `removed` | `fonts` | The typeface and style, e.g. `"Lato Bold"`. |
| `removed` | `orphans` | `""`. |
| `cropped` | `crop` | Pixel size before and after, e.g. `"2500x1667 -> 1493x1667"`. |
| `downsampled` | `images` | Pixel size before and after, e.g. `"1200x900 -> 384x288"`. |
| `recompressed` | `images` | Pixel size (unchanged), e.g. `"1426x513"`. |
| `skipped` | `crop`, `images` | Why the image was left alone, e.g. `"2 uses with different crops"`, `"EXIF orientation 6"`, `"CMYK JPEG"`, `"tiled fill"`. |

An image the pass looked at and left alone without a reason worth reporting
(too small to save at least 1 KiB, already at its displayed size and well
compressed, a format it doesn't re-encode) has no action at all. `optimize`
never rewrites a part to make it larger, and never re-encodes vector
(EMF/WMF/SVG), GIF, TIFF, video or audio media: such a part is only ever
removed, when nothing references it any more.

### Ordering

`passes` are in run order. Within a pass, actions are in part-name order,
except `layouts`, which lists layouts first, then masters, then the rest
(each in part-name order), and `fonts`, which follows the deck's
`p:embeddedFontLst`.
For a given deck, options and `pptctl` version the document is reproducible,
except for byte counts of re-encoded parts, which depend on the platform's
zlib.

---

## Exit codes

| Code | Meaning |
|---|---|
| `0` | The deck was optimized (or, with `--dry-run`, measured), even if no pass had anything to do. |
| `1` | No output given (`-o`, `--in-place` or `--dry-run`), `-o` naming the input without `--in-place`, the input could not be read as a `.pptx`, or the output could not be written. The message goes to stderr; nothing is written to stdout, and a failed write leaves no partial file behind. |
| other | A command-line error caught by the argument parser (unknown flag, `--quality` out of range, `-o` together with `--in-place`): CLI11's own non-zero code, with usage on stderr. |

---

## Example

`pptctl optimize --dry-run --json optimize_deck.pptx` (abridged: the
`layouts` pass removes 11 layouts, a master and a theme, and the `images`
pass downsamples two images; one of each is shown):

```json
{
  "dry_run": true,
  "input": "optimize_deck.pptx",
  "input_bytes": 80715,
  "output": null,
  "output_bytes": 39869,
  "passes": [
    {
      "actions": [
        {
          "bytes_after": 0,
          "bytes_before": 4357,
          "detail": "layout",
          "kind": "removed",
          "part": "ppt/slideLayouts/slideLayout1.xml"
        },
        {
          "bytes_after": 0,
          "bytes_before": 11544,
          "detail": "master",
          "kind": "removed",
          "part": "ppt/slideMasters/slideMaster2.xml"
        }
      ],
      "id": "layouts"
    },
    {
      "actions": [
        {
          "bytes_after": 0,
          "bytes_before": 6000,
          "detail": "Lato",
          "kind": "removed",
          "part": "ppt/fonts/font1.fntdata"
        }
      ],
      "id": "fonts"
    },
    {
      "actions": [
        {
          "bytes_after": 10753,
          "bytes_before": 26448,
          "detail": "800x600 -> 408x600",
          "kind": "cropped",
          "part": "ppt/media/image3.jpg"
        }
      ],
      "id": "crop"
    },
    {
      "actions": [
        {
          "bytes_after": 19189,
          "bytes_before": 51604,
          "detail": "1200x900 -> 384x288",
          "kind": "downsampled",
          "part": "ppt/media/image1.jpg"
        }
      ],
      "id": "images"
    },
    {
      "actions": [
        {
          "bytes_after": 0,
          "bytes_before": 134,
          "detail": "",
          "kind": "removed",
          "part": "ppt/media/orphan.png"
        }
      ],
      "id": "orphans"
    }
  ],
  "schema_version": 1
}
```

Keys are emitted in sorted order (as shown) because the serializer uses an
ordered map; consumers should not depend on key order.
