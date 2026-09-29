// Diff engine — picture-level ops. See diff_internal.hpp.

#include "diff_internal.hpp"

namespace pptctl::detail {

void diff_pictures(const Slide& /*a*/,
                   const Slide& /*b*/,
                   const DiffOptions& /*opts*/,
                   std::vector<PictureChange>& /*out*/) {
    // Filled in by its owning issue.
}

}  // namespace pptctl::detail
