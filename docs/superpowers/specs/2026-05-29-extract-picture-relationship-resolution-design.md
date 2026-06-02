# Design: extract — picture extraction with relationship resolution

- **Issue:** #8 (Phase 2 — extract, v0.2.0)
- **Branch:** `8-extract-picture-extraction-with-relationship-resolution`
- **Date:** 2026-05-29
- **Builds on:** #7 (slide content parsing) and #4 (package relationship resolution)

## Goal

Fill the picture slot left typed-but-shallow by #7. For each `p:pic` in a
slide's shape tree, resolve its `a:blip@r:embed` relationship id through the
per-part relationship graph to the media part it points at, and carry image
metadata (resolved part, content type, byte size, alt text) on the shape model.

This completes the parsing layer for the most common non-text shape and proves
out the relationship-resolution foundation (#4) end-to-end, before output
formats (#12) and golden tests (#14) lock anything down.

## Scope decisions (resolved during brainstorming)

1. **Metadata only — no byte export.** Resolve pictures to their media parts and
   emit metadata. Writing media bytes to a directory is deferred: it pairs
   naturally with the output-format work (#12) / a future `--extract-media` flag,
   and would otherwise pull in directory handling and name-collision concerns.
2. **Metadata fields:** resolved target part (the core of the issue), plus
   content type, alt text, and byte size. All four are cheap and useful (byte
   size also feeds `optimize` later).
3. **Embedded *and* linked images.** Capture both `a:blip@r:embed` (internal) and
   `a:blip@r:link` (external). Resolve internal targets; mark linked images
   external with no resolved part. Mirrors how `Package::relationships` already
   flags external targets.
4. **Byte size from ZIP metadata, not decompression.** Read the uncompressed size
   off the ZIP entry record rather than reading the media bytes, honoring the
   lazy/large-deck memory philosophy.

## Architecture

Two-phase resolution, splitting "parse the XML" from "resolve against the
package" along the layer boundary that already exists:

- `Slide::parse(xml)` stays **package-free** (the existing unit-test seam). When
  it encounters a `p:pic`, it captures the raw relationship id and whether it
  came from `r:embed` or `r:link`, plus alt text. No resolution happens here.
- `Slide::load(pkg, part)` calls `parse()` then, since it alone has the package
  and the source part name, walks the shape tree and resolves each embedded
  picture's rId → media part, content type, and size.

Approaches considered and rejected:

- **Resolve inside `parse()`** (pass `Package` into the parser): simpler call
  path, but couples the XML parser to the package and breaks the package-free
  `parse()` tests. Rejected — the layer split is load-bearing in this codebase.
- **Lazy resolve-on-access** (shape stores rId + package back-pointer, resolves
  on demand): lifetime hazards and over-engineering for a read-once dump. YAGNI.

## Data model (`include/pptctl/slide.hpp`)

A new struct, attached only to `Picture` shapes:

```cpp
// A picture's blip reference and its resolved media part. Populated only on
// shapes with kind == Picture.
struct Picture {
    std::string rel_id;                       // a:blip@r:embed, or @r:link when linked
    bool linked = false;                      // from r:link (external) rather than r:embed
    std::string target;                       // resolved media part; empty if linked/unresolved
    std::string content_type;                 // e.g. "image/png"; empty if unknown
    std::optional<std::uint64_t> size_bytes;  // uncompressed media size; nullopt if linked/unknown
    std::optional<std::string> alt_text;      // p:cNvPr@descr
};
```

`Shape` gains one field:

```cpp
std::optional<Picture> picture;  // populated only when kind == Picture
```

`parse()` fills `rel_id`, `linked`, and `alt_text`. `load()` fills `target`,
`content_type`, and `size_bytes` for non-linked pictures; linked pictures keep
an empty `target` and `nullopt` size.

## Package extension (`include/pptctl/package.hpp` / `src/lib/package.cpp`)

```cpp
// Uncompressed size of a part, read from the ZIP entry metadata without
// decompressing it. nullopt if the part doesn't exist or no package is open.
[[nodiscard]] std::optional<std::uint64_t> part_size(std::string_view name) const;
```

Implementation: locate the entry, then read the uncompressed length from the
entry info (`mz_zip_reader_entry_save_buffer_length`, which returns the stored
uncompressed size — no decompression). Reusable by `info`/`optimize` later.

## Parsing (`src/lib/slide.cpp`)

Replace the shallow `p:pic` branch in `parse_sp_tree`. A `parse_pic(el)` helper:

- `set_identity(el, shape)` as today (id, name, geometry from `p:spPr/a:xfrm`).
- `shape.kind = ShapeKind::Picture`.
- Build `Picture`:
  - `p:blipFill/a:blip` → if `@r:embed` present, `rel_id` = it, `linked = false`;
    else if `@r:link` present, `rel_id` = it, `linked = true`.
  - `alt_text` from `p:nvPicPr/p:cNvPr@descr` when non-empty.
- `shape.picture = std::move(pic)`.

pugixml is loaded with `parse_default`, so namespace prefixes are literal
(`r:embed`, `r:link`) — consistent with how the rels parser reads attributes.

## Resolution (`src/lib/slide.cpp`, in `Slide::load`)

After `parse()` succeeds, walk `shapes_` (recursing into `Group` children) and
for every shape with a `picture` that is not `linked` and has a non-empty
`rel_id`:

- `target = pkg.resolve(part_name_, rel_id)`
- if `target` non-empty: `content_type = pkg.content_type(target)`,
  `size_bytes = pkg.part_size(target)`

Linked pictures and unresolved ids are left as parsed (empty target). A private
recursive helper `resolve_pictures(const Package&)` keeps `load()` tidy and
mirrors the existing `parse_sp_tree` recursion shape.

## `extract` command (`src/cmd/extract.cpp`)

Extend `print_shape` for `Picture` shapes. Instead of the bare
`[picture] "<name>"`, show the resolution:

```
  [picture] "Picture 2" → ppt/media/image1.png (image/png, 120 B)
  [picture] "Logo" → linked (external)
  [picture] "Broken" → unresolved (rId9)
```

Rules:

- Resolved embedded picture: `→ <target> (<content_type>, <size>)`. Omit the
  content type or size segment if unknown.
- Linked picture: `→ linked (external)`.
- Embedded but unresolved (empty target): `→ unresolved (<rel_id>)`.
- Append ` — <alt_text>` when alt text is present.

Byte size formatting is a small local helper (`B`/`KB`/`MB`), cosmetic and not a
stable contract (formats land in #12).

## Testing

`tests/test_slide.cpp` (parse + load) and `tests/test_package.cpp`:

- **parse-level, embedded** (inline `p:pic` XML): `picture` is set,
  `rel_id == "rId2"`, `linked == false`, `alt_text == "image.png"`,
  `target` empty (no resolution in `parse`).
- **parse-level, linked** (inline `p:pic` with `a:blip@r:link="rId5"`):
  `linked == true`, `rel_id == "rId5"`.
- **load-level** against `images_deck.pptx` slide 1: the picture shape resolves
  to `target == "ppt/media/image1.png"`, `content_type == "image/png"`,
  `size_bytes == 120` (the fixture's known media size).
- **`Package::part_size`**: a known part returns its size; a missing part returns
  `nullopt`.

No golden / byte-compared output files — that's #14.

## Out of scope (explicit)

- Exporting media bytes to disk / an `--extract-media` flag.
- Output formats and the `--format` flag (#12); versioned schema (#13); golden
  tests (#14).
- Image pixel dimensions / decoding image headers (only geometry from the slide
  and byte size are reported).
- Table cells (#9), chart data (#10), speaker notes (#11).
- De-duplicating shared media across slides, and `srcRect`/crop/effects on the
  blip.
- Mutating/serializing — this layer stays read-only.
