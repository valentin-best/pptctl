// integrity_dangling_rel.cpp — the "dangling-rel" rule: relationships whose
// target part is missing (#129).

#include <format>
#include <set>
#include <string>
#include <string_view>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

// "http://…/relationships/image" -> "image".
std::string_view short_type(std::string_view type) {
    const auto slash = type.rfind('/');
    return slash == std::string_view::npos ? type : type.substr(slash + 1);
}

}  // namespace

// Every `.rels` in the package is checked, reachable or not, in part-name
// order. A `.rels` whose source part is itself missing is not: that is an
// orphan (orphan-part reports it), and its targets mean nothing.
void integrity_dangling_rel(const Package& pkg,
                            const PartGraph& /*graph*/,
                            const LintOptions& /*opts*/,
                            std::vector<Diagnostic>& out) {
    const auto names = pkg.part_names();
    const std::set<std::string, std::less<>> parts(names.begin(), names.end());
    for (const auto& rels_part : parts) {
        const auto source = source_of_rels(rels_part);
        if (!source || (!source->empty() && !parts.contains(*source))) {
            continue;
        }
        for (const auto& rel : pkg.relationships(*source)) {
            if (rel.external || parts.contains(rel.target)) {
                continue;
            }
            out.push_back({"dangling-rel",
                           Severity::Error,
                           0,
                           "",
                           std::format(R"(relationship {} ({}) targets "{}", which does not exist)",
                                       rel.id,
                                       short_type(rel.type),
                                       rel.target),
                           rels_part});
        }
    }
}

}  // namespace pptctl::detail
