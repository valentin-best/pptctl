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

// #74 — table diff (cells, rows/columns added or removed, merges).
void diff_tables(const Slide& a,
                 const Slide& b,
                 const DiffOptions& opts,
                 std::vector<TableChange>& out);

// #74 — chart diff (type, title, series, categories, values).
void diff_charts(const Slide& a,
                 const Slide& b,
                 const DiffOptions& opts,
                 std::vector<ChartChange>& out);

// #74 — the chart comparison for one matched pair of chart shapes. Exposed
// for tests: chart data is only filled by Slide::load(), so hand-built Shapes
// are the only way to unit-test the comparison without a package. Either side
// without chart data yields nothing.
void diff_chart_pair(const Shape& a, const Shape& b, std::vector<ChartChange>& out);

}  // namespace pptctl::detail
