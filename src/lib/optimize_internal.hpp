// Internal seams between the optimize pipeline and its passes. Not installed;
// each pass implements exactly one of these in src/lib/optimize_<id>.cpp and
// owns that file and tests/test_optimize_<id>.cpp.

#pragma once

#include <pptctl/optimize.hpp>

namespace pptctl::detail {

// #100 — remove slide layouts no slide uses, and masters left with none.
PassResult optimize_layouts(const Package& pkg,
                            const PartGraph& graph,
                            const OptimizeOptions& opts);

// #101 — strip embedded fonts (p:embeddedFontLst and the font parts).
PassResult optimize_fonts(const Package& pkg, const PartGraph& graph, const OptimizeOptions& opts);

// #104 — crop images to their a:srcRect and reset the crop.
PassResult optimize_crop(const Package& pkg, const PartGraph& graph, const OptimizeOptions& opts);

// #103 — recompress and downsample raster images to their displayed size.
PassResult optimize_images(const Package& pkg, const PartGraph& graph, const OptimizeOptions& opts);

// #99 — remove media nothing references.
PassResult optimize_orphans(const Package& pkg,
                            const PartGraph& graph,
                            const OptimizeOptions& opts);

}  // namespace pptctl::detail
