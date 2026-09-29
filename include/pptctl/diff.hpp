// pptctl/diff.hpp
//
// Diff engine: the structured report `pptctl diff` produces and the slide
// matching that builds it. The report is a tree diff — slide-level ops here,
// shape/text/picture ops nested under changed slides by later work — and the
// unified-text, JSON and HTML outputs are all renderers of this one model.
//
// Matching is lazy-friendly: each slide is reduced to a SlideSummary once
// (fingerprint + token bag) and the full Slide is dropped, so two 1000-slide
// decks cost O(n) summaries in memory, not O(n) parsed slide trees.

#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

// Cheap per-slide descriptor used for matching. `fingerprint` covers every
// compared property (shape ids/kinds/geometry, text, picture targets, notes),
// so equal fingerprints mean "unchanged". `tokens` feeds the similarity score
// for best-match pairing when fingerprints differ: words of the text plus
// shape names and picture targets, so image-only slides still compare.
struct SlideSummary {
    std::size_t index = 0;  // 1-based position in its deck
    std::uint64_t fingerprint = 0;
    std::vector<std::string> tokens;  // sorted
};

[[nodiscard]] SlideSummary summarize(const Slide& slide);

// Dice coefficient over the two token multisets, in [0, 1]. Two slides with
// no tokens at all score 1 (nothing to disagree on).
[[nodiscard]] double slide_similarity(const SlideSummary& a, const SlideSummary& b);

enum class SlideStatus : std::uint8_t { Unchanged, Changed, Added, Removed };

// ── nested ops inside a changed slide ─────────────────────────────────────────
//
// Each op family lives in its own source file (src/lib/diff_<family>.cpp) and
// its own test file (tests/test_diff_<family>.cpp), and is computed by
// compare_slides() once per matched pair. Keep the families independent: a
// text op never reaches into shape geometry, and vice versa.

// Text-level change within a matched shape (or the notes). Owned by #70.
//
// Shapes are matched across the pair by id, then name, then placeholder
// type+idx (group children included); each matched pair's paragraphs are
// aligned by LCS over their text. Whitespace-only differences and field runs
// (a:fld) are ignored unless DiffOptions::strict_text is set.
enum class TextChangeKind : std::uint8_t {
    Inserted,     // paragraph only in B
    Deleted,      // paragraph only in A
    Changed,      // paragraph text differs
    Reformatted,  // same text, run formatting differs (see `formatting`)
};

struct TextChange {
    TextChangeKind kind = TextChangeKind::Changed;
    std::string shape;            // p:cNvPr@name of the B shape; "" for notes
    std::string shape_id;         // p:cNvPr@id of the B shape; "" for notes
    bool notes = false;           // true when the change is in the speaker notes
    std::size_t a_paragraph = 0;  // 1-based paragraph index in A; 0 when Inserted
    std::size_t b_paragraph = 0;  // 1-based paragraph index in B; 0 when Deleted
    std::string old_text;         // A paragraph text; empty when Inserted
    std::string new_text;         // B paragraph text; empty when Deleted
    std::string formatting;       // Reformatted only: `"world": bold on, size 18pt -> 24pt`
};

// One-line human-readable summary, e.g.
//   shape "Title 1": paragraph 2 changed: "old" -> "new"
//   notes: paragraph 1 inserted: "Remember to pause"
[[nodiscard]] std::string describe(const TextChange& c);

// Stable snake_case keys: kind ("inserted" | "deleted" | "changed" |
// "reformatted"), shape, shape_id, notes, a_paragraph, b_paragraph, old_text,
// new_text, formatting. Inapplicable fields are null (a 0 index, the absent
// side's text, formatting on a non-Reformatted op).
[[nodiscard]] nlohmann::json to_json(const TextChange& c);

// Structural change to a shape: added, removed, moved, resized, recolored.
// Owned by #72.
//
// Shapes are matched between the two slides by id, then name, then (for
// placeholders) type/idx, recursing into matched groups. One matched pair can
// yield several ops (a shape both moved and recolored gives two entries); each
// op carries everything a renderer needs, in EMU — convert to inches at output.
enum class ShapeOp : std::uint8_t { Added, Removed, Moved, Resized, KindChanged, Recolored };

struct ShapeChange {
    ShapeOp op = ShapeOp::Added;
    std::string id;                       // p:cNvPr@id, from B (from A when Removed)
    std::string name;                     // p:cNvPr@name, from B (from A when Removed)
    ShapeKind kind = ShapeKind::Other;    // kind in B (in A when Removed); new kind for KindChanged
    std::optional<ShapeKind> old_kind;    // KindChanged only
    std::vector<std::string> group_path;  // names of enclosing groups, outermost first
    std::optional<Geometry> old_geometry;  // A side; nullopt when Added or A has no a:xfrm
    std::optional<Geometry> new_geometry;  // B side; nullopt when Removed or B has no a:xfrm
    Emu delta_x = 0;                       // Moved: new.off_x - old.off_x
    Emu delta_y = 0;                       // Moved: new.off_y - old.off_y
    Emu delta_cx = 0;                      // Resized: new.ext_cx - old.ext_cx
    Emu delta_cy = 0;                      // Resized: new.ext_cy - old.ext_cy
    std::optional<Fill> old_fill;          // A side (Recolored, Removed); nullopt when inherited
    std::optional<Fill> new_fill;          // B side (Recolored, Added); nullopt when inherited
};

// Stable lower-snake-case label: "added", "removed", "moved", "resized",
// "kind_changed", "recolored".
[[nodiscard]] std::string_view to_string(ShapeOp op) noexcept;

// One human-readable line, inches with two decimals, e.g.
//   shape "Picture 3" moved by (+0.25 in, -0.10 in)
//   shape "Child" in group "Group 1" resized by (+1.00 in, -0.50 in) to 2.00 x 0.50 in
[[nodiscard]] std::string describe(const ShapeChange& change);

// Machine-readable form with stable snake_case keys. Every key is always
// present (null when not applicable to the op): op, id, name, kind, group_path,
// old_kind, old_geometry, new_geometry, delta, old_fill, new_fill. Geometry and
// delta objects carry EMU as integers (x_emu, y_emu, cx_emu, cy_emu) alongside
// inches as doubles (x_in, y_in, cx_in, cy_in).
[[nodiscard]] nlohmann::json to_json(const ShapeChange& change);

// Picture media change: added, removed, replaced, re-linked, alt-text.
// Owned by #73.
//
// Pictures are paired across the two slides by shape id, then by shape name
// (groups are walked). Media identity is the content hash (Picture::media_hash)
// when the picture resolved, so the same bytes under different part names are
// not a change and different bytes under the same name are; unresolved
// pictures fall back to the relationship target. One matched pair can yield
// more than one op (e.g. Replaced and AltTextChanged), emitted in that order.
enum class PictureOp : std::uint8_t {
    Added,           // present only in B
    Removed,         // present only in A
    Replaced,        // same shape, different media
    Relinked,        // embedded <-> linked (external)
    AltTextChanged,  // alt text, title or decorative marker differs
};

struct PictureChange {
    PictureOp op = PictureOp::Added;
    std::string shape_id;           // p:cNvPr@id — B's when the shape exists there, else A's
    std::string shape_name;         // p:cNvPr@name, same side as shape_id
    std::optional<Picture> before;  // A side; nullopt when Added
    std::optional<Picture> after;   // B side; nullopt when Removed
};

// Stable snake_case name of the op ("added", "alt_text_changed", ...), as used
// by to_json.
[[nodiscard]] std::string_view to_string(PictureOp op) noexcept;

// One-line human summary, e.g.
//   picture "Picture 2" replaced (image1.png 120 B 64x48 -> image7.jpg 2.3 MiB 1920x1080)
[[nodiscard]] std::string describe(const PictureChange& change);

// {"op", "shape_id", "shape_name", "before", "after"}; each side is null or
// {"rel_id", "linked", "target", "content_type", "size_bytes", "media_hash",
// "width", "height", "alt_text", "title", "decorative"} with null for unknowns.
// media_hash is a 16-digit hex string (a uint64 is not JSON-safe).
[[nodiscard]] nlohmann::json to_json(const PictureChange& change);

struct SlideChanges {
    std::vector<TextChange> text;
    std::vector<ShapeChange> shapes;
    std::vector<PictureChange> pictures;

    [[nodiscard]] bool empty() const noexcept {
        return text.empty() && shapes.empty() && pictures.empty();
    }
};

struct SlideDiff {
    SlideStatus status = SlideStatus::Unchanged;
    std::size_t a_index = 0;  // 1-based in A; 0 when Added
    std::size_t b_index = 0;  // 1-based in B; 0 when Removed
    bool moved = false;       // matched slide whose order relative to its peers changed
    double similarity = 1.0;  // 1 for exact matches; best-match score otherwise
    SlideChanges changes;     // filled for Changed slides; empty otherwise
};

struct DiffReport {
    std::vector<SlideDiff> slides;  // B order; removed slides slotted at their A position

    [[nodiscard]] bool identical() const noexcept;
};

struct DiffOptions {
    // Minimum similarity for two non-identical slides to be paired as
    // "changed" rather than reported as one removed and one added.
    double match_threshold = 0.5;

    // Position/size deltas at or below this many EMU are not reported as
    // moved/resized, so sub-pixel jitter from a re-save stays quiet. The
    // default is 1 px at 96 DPI (914400 / 96); 0 reports every change.
    Emu geometry_tolerance_emu = 9525;
    // Text diff (#70): report whitespace-only differences and field-run
    // (a:fld) churn instead of ignoring them.
    bool strict_text = false;
};

// Pair the slides of A with the slides of B: exact fingerprints first (so a
// reorder is cheap), then best-match by similarity above the threshold, then
// whatever is left is added/removed. Matched pairs whose relative order
// differs are flagged moved (longest increasing subsequence of B positions).
[[nodiscard]] DiffReport match_slides(const std::vector<SlideSummary>& a,
                                      const std::vector<SlideSummary>& b,
                                      const DiffOptions& opts);

// Compute the nested ops for one matched pair. Runs each op family in turn;
// the command layer calls this in a second pass that re-loads only the pairs
// match_slides() reported as Changed, so unchanged slides are parsed once.
[[nodiscard]] SlideChanges compare_slides(const Slide& a, const Slide& b, const DiffOptions& opts);

// ── command-layer output settings (#71) ───────────────────────────────────────
//
// Read only by src/cmd/diff.cpp — the renderers, not the engine. Kept apart
// from DiffOptions so the matching/comparison knobs stay a library concern.
struct DiffOutputOptions {
    std::string format = "unified";  // unified | json | html
    bool quiet = false;              // print nothing; exit code only
    std::optional<bool> color;       // unified: nullopt = auto (TTY and no NO_COLOR)
};

}  // namespace pptctl
