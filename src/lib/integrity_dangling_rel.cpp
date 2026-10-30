// integrity_dangling_rel.cpp — the "dangling-rel" rule: relationships whose target part is missing
// (#129).

#include "integrity_internal.hpp"

namespace pptctl::detail {

// No-op until #129 lands.
void integrity_dangling_rel(const Package& /*pkg*/,
                            const PartGraph& /*graph*/,
                            const LintOptions& /*opts*/,
                            std::vector<Diagnostic>& /*out*/) {}

}  // namespace pptctl::detail
