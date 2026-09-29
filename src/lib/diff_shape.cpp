// Diff engine — shape-level ops. See diff_internal.hpp.

#include "diff_internal.hpp"

namespace pptctl::detail {

void diff_shapes(const Slide& /*a*/,
                 const Slide& /*b*/,
                 const DiffOptions& /*opts*/,
                 std::vector<ShapeChange>& /*out*/) {
    // Filled in by its owning issue.
}

}  // namespace pptctl::detail
