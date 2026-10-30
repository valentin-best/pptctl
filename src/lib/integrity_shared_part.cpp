// integrity_shared_part.cpp — the "shared-part" rule: exclusively-owned parts
// targeted from several owners (#133).

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl {

namespace {

constexpr std::string_view kOfficeRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kMsRel = "http://schemas.microsoft.com/office/";

// Relationship types whose target must have a single owner. Everything else
// (media, layouts, masters, themes, fonts, ...) is legitimately shared.
struct ExclusiveType {
    std::string_view base;
    std::string_view name;
};
constexpr std::array<ExclusiveType, 9> kExclusiveTypes = {{
    // A notes slide belongs to one slide; it points back at it.
    {kOfficeRel, "notesSlide"},
    // Comment lists, legacy and modern, are per slide.
    {kOfficeRel, "comments"},
    {kMsRel, "2018/10/relationships/comments"},
    // Per-shape or per-slide add-in data; PowerPoint won't open a deck where
    // two shapes share one tags part (OfficeCLI#540).
    {kOfficeRel, "tags"},
    // A chart part is one graphic frame's chart, and its style and colour
    // parts are that chart's: PowerPoint won't open a clone that shares them
    // with its source (OfficeCLI#541).
    {kOfficeRel, "chart"},
    {kMsRel, "2014/relationships/chartEx"},
    {kMsRel, "2011/relationships/chartStyle"},
    {kMsRel, "2011/relationships/chartColorStyle"},
    // An embedded workbook or OLE package: "Edit Data" on one owner would
    // rewrite the other's.
    {kOfficeRel, "package"},
}};

// "http://…/relationships/chartStyle" -> "chartStyle".
std::string_view short_type(std::string_view type) {
    const auto slash = type.rfind('/');
    return slash == std::string_view::npos ? type : type.substr(slash + 1);
}

// How many times each rId is used in a source part's XML, parsed on first ask.
class RefCounts {
 public:
    explicit RefCounts(const Package& pkg) : pkg_(pkg) {}

    std::size_t uses(const std::string& source, const std::string& rid) {
        auto it = counts_.find(source);
        if (it == counts_.end()) {
            std::map<std::string, std::size_t, std::less<>> per_id;
            if (!source.empty()) {
                for (const auto& ref : detail::relationship_refs(pkg_.read_part(source))) {
                    ++per_id[ref.id];
                }
            }
            it = counts_.emplace(source, std::move(per_id)).first;
        }
        const auto use = it->second.find(rid);
        return use == it->second.end() ? 0 : use->second;
    }

 private:
    const Package& pkg_;
    std::map<std::string, std::map<std::string, std::size_t, std::less<>>, std::less<>> counts_;
};

}  // namespace

bool is_exclusively_owned(std::string_view rel_type) {
    return std::any_of(kExclusiveTypes.begin(), kExclusiveTypes.end(), [&](const auto& t) {
        return rel_type.size() == t.base.size() + t.name.size() && rel_type.starts_with(t.base) &&
               rel_type.ends_with(t.name);
    });
}

namespace detail {

// For each reachable part, its inbound references of an exclusive type are
// grouped by source. A source owns the part once per XML use of the rIds that
// reach it (two shapes using one tags rId are two owners), or once when the
// relationship is implicit and never named in XML (a slide's notes). Two or
// more owners in all is a finding, located at the shared part.
void integrity_shared_part(const Package& pkg,
                           const PartGraph& graph,
                           const LintOptions& /*opts*/,
                           std::vector<Diagnostic>& out) {
    RefCounts ref_counts(pkg);
    for (const auto& target : graph.reachable()) {
        struct Owner {
            std::string source;
            std::size_t uses = 0;
        };
        std::vector<Owner> owners;
        std::string_view type;
        for (const auto& ref : graph.references_to(target)) {
            if (!is_exclusively_owned(ref.rel.type)) {
                continue;
            }
            type = short_type(ref.rel.type);
            auto owner = std::find_if(owners.begin(), owners.end(), [&](const Owner& o) {
                return o.source == ref.source;
            });
            if (owner == owners.end()) {
                owners.push_back({ref.source, 0});
                owner = std::prev(owners.end());
            }
            owner->uses += ref_counts.uses(ref.source, ref.rel.id);
        }
        std::size_t total = 0;
        std::string who;
        for (const auto& o : owners) {
            const std::size_t n = std::max<std::size_t>(o.uses, 1);
            total += n;
            who += (who.empty() ? "" : ", ") + o.source;
            if (n > 1) {
                who += std::format(" ({} references)", n);
            }
        }
        if (total < 2) {
            continue;
        }
        out.push_back(
            {"shared-part",
             Severity::Error,
             0,
             "",
             std::format(
                 R"({} part "{}" is shared by {}; each needs its own copy)", type, target, who),
             target});
    }
}

}  // namespace detail

}  // namespace pptctl
