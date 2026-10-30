// integrity_broken_rid.cpp — the "broken-rid" rule: r:embed / r:id / r:link
// not defined in the part's .rels (#130).

#include <cstddef>
#include <format>
#include <map>
#include <pptctl/presentation.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

bool is_xml_content_type(std::string_view ct) {
    return ct.ends_with("+xml") || ct.ends_with("/xml");
}

// Slide part -> 1-based slide number, numbered the way lint numbers slides.
// Empty when presentation.xml doesn't load: findings then carry slide 0.
std::map<std::string, std::size_t, std::less<>> slide_numbers(const Package& pkg) {
    std::map<std::string, std::size_t, std::less<>> out;
    Presentation pres(pkg);
    if (pres.load()) {
        for (std::size_t i = 0; i < pres.slide_parts().size(); ++i) {
            out.emplace(pres.slide_parts()[i], i + 1);
        }
    }
    return out;
}

}  // namespace

// Every reachable XML part under ppt/, one parsed at a time. Unreachable parts
// are left to orphan-part; parts that don't parse have no references.
void integrity_broken_rid(const Package& pkg,
                          const PartGraph& graph,
                          const LintOptions& /*opts*/,
                          std::vector<Diagnostic>& out) {
    const auto slides = slide_numbers(pkg);
    for (const auto& part : graph.reachable()) {
        if (!part.starts_with("ppt/") || !is_xml_content_type(pkg.content_type(part))) {
            continue;
        }
        const auto refs = relationship_refs(pkg.read_part(part));
        if (refs.empty()) {
            continue;
        }
        std::set<std::string, std::less<>> defined;
        for (const auto& rel : pkg.relationships(part)) {
            defined.insert(rel.id);
        }
        const auto slide = slides.find(part);
        for (const auto& ref : refs) {
            if (defined.contains(ref.id)) {
                continue;
            }
            out.push_back(
                {"broken-rid",
                 Severity::Error,
                 slide == slides.end() ? 0 : slide->second,
                 "",
                 std::format(
                     R"({}="{}" is not defined in {})", ref.qname, ref.id, rels_part_for(part)),
                 part});
        }
    }
}

}  // namespace pptctl::detail
