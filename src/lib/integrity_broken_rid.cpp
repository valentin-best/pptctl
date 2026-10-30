// integrity_broken_rid.cpp — the "broken-rid" rule: r:embed / r:id / r:link not defined in the
// part's .rels (#130).

#include "integrity_internal.hpp"

namespace pptctl::detail {

// No-op until #130 lands.
void integrity_broken_rid(const Package& /*pkg*/,
                          const PartGraph& /*graph*/,
                          const LintOptions& /*opts*/,
                          std::vector<Diagnostic>& /*out*/) {}

}  // namespace pptctl::detail
