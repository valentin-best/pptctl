// integrity_orphan_part.cpp — the "orphan-part" rule: parts no relationship reaches (#132).

#include "integrity_internal.hpp"

namespace pptctl::detail {

// No-op until #132 lands.
void integrity_orphan_part(const Package& /*pkg*/,
                           const PartGraph& /*graph*/,
                           const LintOptions& /*opts*/,
                           std::vector<Diagnostic>& /*out*/) {}

}  // namespace pptctl::detail
