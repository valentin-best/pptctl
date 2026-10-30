// Internal seams between the integrity registry and its rules. Not installed;
// each rule implements exactly one of these in src/lib/integrity_<id>.cpp and
// owns that file and tests/test_integrity_<id>.cpp.

#pragma once

#include <cstddef>
#include <optional>
#include <pptctl/integrity.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::detail {

// The part a `.rels` part declares relationships for ("" for the package
// root's `_rels/.rels`), or nullopt when `name` isn't a relationships part.
// The inverse of rels_part_for.
[[nodiscard]] std::optional<std::string> source_of_rels(std::string_view name);

// One relationship-id use in a part's XML: an attribute in the relationships
// namespace (Transitional or Strict), e.g. r:embed="rId4".
struct RelationshipRef {
    std::string qname;  // attribute name as written, e.g. "r:embed"
    std::string id;     // its value, e.g. "rId4"
};

// Every relationship-id use in `xml`, in document order. The namespace is
// matched by URI through the in-scope xmlns: declarations, not by the "r:"
// prefix; empty values aren't references. Empty if `xml` doesn't parse.
[[nodiscard]] std::vector<RelationshipRef> relationship_refs(std::span<const std::byte> xml);

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
