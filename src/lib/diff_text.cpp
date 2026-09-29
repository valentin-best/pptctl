// Diff engine — text-level ops. See diff_internal.hpp.

#include "diff_internal.hpp"

namespace pptctl::detail {

void diff_text(const Slide& /*a*/,
               const Slide& /*b*/,
               const DiffOptions& /*opts*/,
               std::vector<TextChange>& /*out*/) {
    // Filled in by its owning issue.
}

}  // namespace pptctl::detail
