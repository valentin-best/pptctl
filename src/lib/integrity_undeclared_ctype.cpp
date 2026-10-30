// integrity_undeclared_ctype.cpp — the "undeclared-ctype" rule: parts missing from
// [Content_Types].xml (#131).

#include "integrity_internal.hpp"

namespace pptctl::detail {

// No-op until #131 lands.
void integrity_undeclared_ctype(const Package& /*pkg*/,
                                const PartGraph& /*graph*/,
                                const LintOptions& /*opts*/,
                                std::vector<Diagnostic>& /*out*/) {}

}  // namespace pptctl::detail
