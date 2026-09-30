// Internal seams between the diff engine and its op families. Not installed;
// each family implements exactly one of these in src/lib/diff_<family>.cpp.

#pragma once

#include <pptctl/diff.hpp>
#include <vector>

namespace pptctl::detail {

// #70 — text-level diff over matched shapes' paragraphs and the notes.
void diff_text(const Slide& a,
               const Slide& b,
               const DiffOptions& opts,
               std::vector<TextChange>& out);

// #72 — shape-level structural diff (added/removed/moved/resized/recolored).
void diff_shapes(const Slide& a,
                 const Slide& b,
                 const DiffOptions& opts,
                 std::vector<ShapeChange>& out);

// #73 — picture media diff (added/removed/replaced/re-linked/alt-text).
void diff_pictures(const Slide& a,
                   const Slide& b,
                   const DiffOptions& opts,
                   std::vector<PictureChange>& out);

}  // namespace pptctl::detail
