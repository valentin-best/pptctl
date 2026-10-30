// integrity_unlisted_part.cpp — the "unlisted-part" rule: slides and masters
// the presentation part relates but never lists (#152).

#include <array>
#include <format>
#include <set>
#include <string>
#include <string_view>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

// A relationship type the presentation part must also list, and where.
struct Listed {
    std::string_view type;     // short relationship type
    std::string_view element;  // the list entry naming it by r:id
    std::string_view list;     // the list, as written in the message
    std::string_view why;      // what goes wrong, appended when known
};

constexpr std::array<Listed, 4> kListed = {{
    {"slide", "sldId", "p:sldIdLst", "it is not part of the slide show"},
    {"slideMaster", "sldMasterId", "p:sldMasterIdLst", ""},
    {"notesMaster", "notesMasterId", "p:notesMasterIdLst", "Keynote refuses to open the deck"},
    {"handoutMaster", "handoutMasterId", "p:handoutMasterIdLst", ""},
}};

const Listed* listed_as(std::string_view rel_type) {
    if (!rel_type.starts_with(kRelNs)) {
        return nullptr;
    }
    const auto type = rel_type.substr(kRelNs.size());
    for (const auto& l : kListed) {
        if (l.type == type) {
            return &l;
        }
    }
    return nullptr;
}

}  // namespace

// Each relationship from the presentation part to a slide, slide master,
// notes master or handout master must be named by an entry of the matching
// list in the presentation part. A missing target is dangling-rel's, not this.
void integrity_unlisted_part(const Package& pkg,
                             const PartGraph& /*graph*/,
                             const LintOptions& /*opts*/,
                             std::vector<Diagnostic>& out) {
    std::string pres;
    for (const auto& rel : pkg.relationships("")) {
        if (!rel.external && rel.type == std::string(kRelNs) + "officeDocument") {
            pres = rel.target;
            break;
        }
    }
    if (pres.empty() || !pkg.part_size(pres)) {
        return;
    }
    std::set<std::pair<std::string, std::string>> named;  // (element, rId)
    for (auto& ref : relationship_refs(pkg.read_part(pres))) {
        named.emplace(std::move(ref.element), std::move(ref.id));
    }
    for (const auto& rel : pkg.relationships(pres)) {
        const auto* listed = listed_as(rel.type);
        if (rel.external || listed == nullptr || !pkg.part_size(rel.target) ||
            named.contains({std::string(listed->element), rel.id})) {
            continue;
        }
        auto message =
            std::format(R"(relationship {} ({}) targets "{}", but {} doesn't list it in {})",
                        rel.id,
                        listed->type,
                        rel.target,
                        pres,
                        listed->list);
        if (!listed->why.empty()) {
            message += std::format(": {}", listed->why);
        }
        out.push_back({"unlisted-part", Severity::Error, 0, "", std::move(message), pres});
    }
}

}  // namespace pptctl::detail
