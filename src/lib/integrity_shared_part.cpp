// integrity_shared_part.cpp — the "shared-part" rule: exclusively-owned parts targeted from several
// owners (#133).

#include "integrity_internal.hpp"

namespace pptctl::detail {

// No-op until #133 lands.
void integrity_shared_part(const Package& /*pkg*/,
                           const PartGraph& /*graph*/,
                           const LintOptions& /*opts*/,
                           std::vector<Diagnostic>& /*out*/) {}

}  // namespace pptctl::detail
