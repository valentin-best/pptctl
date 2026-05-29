# Design: extract — slide content parsing (shape tree, text frames, paragraphs, runs)

- **Issue:** #7 (Phase 2 — extract, v0.2.0)
- **Branch:** `7-extract-slide-content-parsing-shape-tree-text-frames-paragraphs-runs`
- **Date:** 2026-05-27

## Goal

Build the document-model layer for slide content: walk a slide's shape tree
(`p:spTree`) and pull out its text structure — shapes, group shapes,
placeholders, text frames, paragraphs, and runs — preserving reading order.

This is the parsing core that the rest of Phase 2 builds on. It deliberately
does **not** own:

- Output formats `--format json|md|txt` → issue #12
- Versioned JSON output schema → issue #13
- Golden tests → issue #14
- Picture relationship resolution → #8; table cells → #9; chart data → #10;
  speaker notes → #11

## Scope decisions (resolved during brainstorming)

1. **Command scope:** build the document model + unit tests, *and* wire the
   `extract` command to a minimal human-readable text dump so the work is
   end-to-end usable. Rich formats and schema stay deferred to #12/#13. Mirrors
   how `info` shipped (model + command together).
2. **Non-text shapes:** represent *every* shape in the tree with a kind tag +
   name + optional geometry, but only populate text content. Pictures, tables,
   charts, diagrams, OLE objects, and connectors get a typed-but-shallow slot so
   the shape tree stays complete and reading order is intact. #8–#10 fill those
   slots later.
3. **Run properties:** capture common formatting — bold, italic, underline, font
   size, and font/typeface. Color is out of scope (overlaps with theme work, and
   scheme-color resolution is fiddly).
4. **Output:** text-only for now. No `--json` flag in #7 — that would pre-empt
   #13's schema work.

## Architecture

A new document-layer unit, `slide.hpp` / `slide.cpp`, sitting on top of the
package layer alongside `presentation.hpp`. pugixml stays internal to the `.cpp`
(same boundary as `presentation.cpp`). The chosen approach is an **eager
value-type model, lazy per slide**: `Slide::load()` parses one slide's XML into
owned plain structs. The deck is never fully materialized — the command loads
slides one at a time via the presentation's ordered slide-part list — which
satisfies the large-deck memory goal without iterator/lifetime complexity.

Approaches considered and rejected:

- **Lazy pugixml cursor** (keep the parsed doc alive, iterate nodes): leaks
  pugixml into the public header, lifetime hazards, hard to unit-test. A single
  slide is small, so the frugality buys nothing here.
- **SAX/visitor callbacks**: every consumer would re-implement traversal and
  ordering; awkward for the typed-shallow tree. Overkill now.

## Data model (`include/pptctl/slide.hpp`)

```cpp
enum class ShapeKind {
    Placeholder, TextBox, Picture, Table, Chart,
    Diagram, Ole, Group, Connector, Other
};

// All optional: an unset field means "inherit" (not explicitly false).
struct RunProperties {
    std::optional<bool>        bold;        // a:rPr@b
    std::optional<bool>        italic;      // a:rPr@i
    std::optional<bool>        underlined;  // a:rPr@u != "none"  (variants collapse to bool)
    std::optional<int>         size_centipt;// a:rPr@sz, hundredths of a point (native unit)
    std::optional<std::string> font;        // a:rPr/a:latin@typeface

    // Convenience, mirrors the EMU→inches pattern in presentation.hpp.
    [[nodiscard]] double size_pt() const;   // size_centipt / 100.0, 0 if unset
};

struct Run {
    std::string   text;
    RunProperties props;
    bool          is_field = false;         // came from a:fld rather than a:r
};

struct Paragraph {
    int              level = 0;             // a:pPr@lvl (bullet/indent depth)
    std::vector<Run> runs;
    [[nodiscard]] std::string text() const; // concatenation of run texts
};

// Present only when the shape carries an a:xfrm. EMU, the native OOXML unit.
struct Geometry {
    Emu off_x = 0, off_y = 0, ext_cx = 0, ext_cy = 0;
};

struct Shape {
    ShapeKind                  kind = ShapeKind::Other;
    std::string                id;                // p:cNvPr@id
    std::string                name;              // p:cNvPr@name
    std::optional<std::string> placeholder_type;  // p:nvPr/p:ph@type, e.g. "ctrTitle"
    std::optional<Geometry>    geometry;
    std::vector<Paragraph>     paragraphs;        // text body; empty for non-text shapes
    std::vector<Shape>         children;          // populated only for Group, recursive
};

class Slide {
 public:
    Slide() = default;
    [[nodiscard]] bool load(const Package& pkg, std::string_view part_name);

    [[nodiscard]] std::size_t index() const noexcept;          // 1-based position in deck
    [[nodiscard]] const std::string& part_name() const noexcept;
    [[nodiscard]] const std::vector<Shape>& shapes() const noexcept; // top-level, document order

    void set_index(std::size_t i) noexcept;  // set by the caller after load
 private:
    std::size_t index_ = 0;
    std::string part_name_;
    std::vector<Shape> shapes_;
};
```

## Parsing (`src/lib/slide.cpp`)

Walk `p:cSld/p:spTree` children **in document order** — that order *is* reading
order. Dispatch by element name:

| Element          | ShapeKind                              | Notes |
|------------------|----------------------------------------|-------|
| `p:sp`           | `Placeholder` if `p:nvPr/p:ph` present, else `TextBox` | parse `p:txBody` |
| `p:grpSp`        | `Group`                                | recurse into children with the same dispatch |
| `p:pic`          | `Picture`                              | no embed resolution (that's #8) |
| `p:graphicFrame` | from `a:graphic/a:graphicData@uri`     | `…/table`→Table, `…/chart`→Chart, `…/diagram`→Diagram, `…/ole`→Ole, else Other |
| `p:cxnSp`        | `Connector`                            | |
| anything else    | `Other`                                | |

Identity (`id`, `name`) comes from the shape's `…/p:cNvPr`. Geometry comes from
`p:spPr/a:xfrm` (`a:off@x/@y`, `a:ext@cx/@cy`) when present; absent for shapes
that inherit position from their placeholder.

**Text body** (`p:txBody`): each `a:p` becomes a `Paragraph` (level from
`a:pPr@lvl`, default 0). Within a paragraph, iterate children in order:

- `a:r` → `Run` (text from `a:r/a:t`, props from `a:r/a:rPr`)
- `a:fld` → `Run` with `is_field = true` (text from `a:fld/a:t`)
- `a:br` → `Run` with `text = "\n"` (line break)

Empty paragraphs (`<a:p/>`) are preserved as empty `Paragraph`s — they carry
structure and reading order.

**Run properties** from `a:rPr`: `@b`/`@i` parse as booleans (`"1"`/`"0"`/
`"true"`/`"false"`); `@u` maps to `underlined = (value != "none")`; `@sz` is an
integer in hundredths of a point; font is `a:rPr/a:latin@typeface`. Each is set
only when the attribute/element is present, so "inherit" stays distinct from
"explicitly off".

## Presentation extension (`presentation.hpp` / `presentation.cpp`)

`Presentation` currently only *counts* slides. Add ordered slide-part
resolution so the command can load slides in true deck order:

- During `load()`, while iterating `p:sldIdLst/p:sldId`, read each `@r:id` and
  resolve it via `package_->resolve("ppt/presentation.xml", rId)` into an
  ordered `std::vector<std::string> slide_parts_`.
- Expose `const std::vector<std::string>& slide_parts() const`.
- `slide_count()` stays as-is (and should equal `slide_parts_.size()`).

This is the lazy handle: the command holds the ordered part names and loads one
`Slide` at a time.

## `extract` command (`src/cmd/extract.cpp`, wired in `main.cpp`)

Replace the `not_implemented("extract")` stub. New file `src/cmd/extract.cpp`
with entrypoint `int pptctl::cmd::extract(const std::string& path)` in
`namespace pptctl::cmd`. Register the subcommand in `main.cpp` (forward-declare,
add CLI11 `add_subcommand("extract", …)` with a required `file` argument checked
by `CLI::ExistingFile`, callback dispatches to the entrypoint) and remove the
placeholder stub for `extract`. Add `src/cmd/extract.cpp` to the `pptctl`
executable's source list in `CMakeLists.txt`.

Behavior: open the package, load the presentation, iterate `slide_parts()`,
load each `Slide`, and print a minimal indented text dump using the
`pptctl::println` helper:

```
slide 1
  [ctrTitle] Lecture 1: Limits, Derivatives, the Product Rule, the Quotient Rule, and the Chain Rule
slide 2
  [picture] "Picture 3"
  • bullet text at level 0
    ◦ nested text at level 1
```

Rules for the dump:

- One `slide N` header per slide (1-based).
- Text-bearing shapes: print each paragraph, indented by its `level`. Prefix a
  placeholder's first line with `[<placeholder_type>]` when present.
- Non-text shapes: print `[<kind>] "<name>"` (e.g. `[picture] "Picture 3"`).
- Group shapes: recurse, printing their children in order.

The exact glyphs/indentation are cosmetic and may change when #12 lands proper
formats; this output exists to make the parsing demonstrable and testable, not
as a stable contract.

Error handling matches `info`: fail with a non-zero exit and an `eprintln`
message if the file can't be opened or isn't a valid `.pptx`.

## Testing (`tests/test_slide.cpp`, added to `pptctl_tests` in `CMakeLists.txt`)

Targeted doctest unit assertions against existing fixtures — no golden files
(that's #14):

- **`three_slides.pptx`**: three slides; slide 1's title shape has a paragraph
  whose `text()` is `"Slide 1"`; reading order of shapes matches the file.
- **`images_deck.pptx`**: slides contain `Picture` shapes; verify a picture
  shape's kind and that reading order places shapes as in the XML.
- **Run properties**: assert bold/size are captured where set (use a fixture or
  real-world slide that has explicit `a:rPr@b`/`@sz`).
- **Group recursion + order**: load a real-world slide with a `p:grpSp`
  (`edu_stanford_cs345d_lecture01` slide 6) and assert the group's children are
  parsed and ordered.
- **Fields/breaks**: assert an `a:fld` run is flagged `is_field` and an `a:br`
  yields a newline run, on a slide that has them.
- **Presentation `slide_parts()`**: returns the right count and order for
  `three_slides.pptx`.

## Out of scope (explicit)

- JSON / markdown / txt formats and the `--format` flag (#12).
- Versioned output schema (#13).
- Golden / byte-compared expected-output files (#14).
- Picture embed/relationship resolution (#8), table cell contents (#9), chart
  data (#10), speaker notes (#11).
- Run color and scheme-color resolution.
- Underline variant fidelity (collapsed to a bool for now).
- Mutating/serializing slides — this layer is read-only.
```
