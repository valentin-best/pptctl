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
#include <pptctl/slide.hpp>
#include <string>
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
struct TextChange {};

// Structural change to a shape: added, removed, moved, resized, recolored.
// Owned by #72.
struct ShapeChange {};

// Picture media change: added, removed, replaced, re-linked, alt-text.
// Owned by #73.
struct PictureChange {};

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

}  // namespace pptctl
