// integrity_dangling_rel.cpp — the "dangling-rel" rule: relationships whose
// target part is missing (#129).

#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kRelsDir = "_rels/";
constexpr std::string_view kRelsExt = ".rels";

// The part a `.rels` part declares relationships for ("" for the package
// root's `_rels/.rels`), or nullopt when `name` isn't a relationships part.
std::optional<std::string> source_of_rels(std::string_view name) {
    if (!name.ends_with(kRelsExt)) {
        return std::nullopt;
    }
    const auto slash = name.rfind('/');
    const std::string_view dir = slash == std::string_view::npos ? "" : name.substr(0, slash + 1);
    if (!dir.ends_with(kRelsDir)) {
        return std::nullopt;
    }
    const std::string_view file =
        name.substr(dir.size(), name.size() - dir.size() - kRelsExt.size());
    std::string source(dir.substr(0, dir.size() - kRelsDir.size()));
    source += file;
    return source;
}

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
