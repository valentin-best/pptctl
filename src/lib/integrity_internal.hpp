// Internal seams between the integrity registry and its rules. Not installed;
// each rule implements exactly one of these in src/lib/integrity_<id>.cpp and
// owns that file and tests/test_integrity_<id>.cpp.

#pragma once

#include <pptctl/integrity.hpp>

namespace pptctl::detail {

// #129 — an internal relationship whose target part is absent.
void integrity_dangling_rel(const Package& pkg,
                            const PartGraph& graph,
                            const LintOptions& opts,
                            std::vector<Diagnostic>& out);

// #130 — a relationship-namespace attribute naming an rId the part's .rels lacks.
void integrity_broken_rid(const Package& pkg,
                          const PartGraph& graph,
                          const LintOptions& opts,
                          std::vector<Diagnostic>& out);

// #131 — a part with no Default or Override in [Content_Types].xml.
void integrity_undeclared_ctype(const Package& pkg,
                                const PartGraph& graph,
                                const LintOptions& opts,
                                std::vector<Diagnostic>& out);

// #132 — a part no relationship reaches.
void integrity_orphan_part(const Package& pkg,
                           const PartGraph& graph,
                           const LintOptions& opts,
                           std::vector<Diagnostic>& out);

// #133 — a part that must have one owner, targeted from several.
void integrity_shared_part(const Package& pkg,
                           const PartGraph& graph,
                           const LintOptions& opts,
                           std::vector<Diagnostic>& out);

}  // namespace pptctl::detail
