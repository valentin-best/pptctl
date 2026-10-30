// merge_theme.cpp — restyling foreign slides onto the --theme deck's layouts (#138).

#include <algorithm>
#include <pugixml.hpp>
#include <string>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kOfficeDocumentRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
constexpr std::string_view kSlideMasterRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster";

// A layout's sldLayout@type (absent means "cust") and p:cSld@name.
std::pair<std::string, std::string> type_and_name(const Package& pkg, const std::string& layout) {
    const auto bytes = pkg.read_part(layout);
    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.data(), bytes.size())) {
        return {"cust", ""};
    }
    const auto root = doc.child("p:sldLayout");
    return {root.attribute("type").as_string("cust"),
            root.child("p:cSld").attribute("name").as_string("")};
}

}  // namespace

LayoutMatcher::LayoutMatcher(const Package& base) {
    std::string pres;
    for (const auto& rel : base.relationships("")) {
        if (!rel.external && rel.type == kOfficeDocumentRel) {
            pres = rel.target;
        }
    }
    for (const auto& rel : base.relationships(pres)) {
        if (rel.external || rel.type != kSlideMasterRel) {
            continue;
        }
        const auto bytes = base.read_part(rel.target);
        pugi::xml_document doc;
        if (!doc.load_buffer(bytes.data(), bytes.size())) {
            continue;
        }
        for (const auto& id :
             doc.child("p:sldMaster").child("p:sldLayoutIdLst").children("p:sldLayoutId")) {
            auto part = base.resolve(rel.target, id.attribute("r:id").as_string(""));
            if (part.empty() || !base.part_size(part)) {
                continue;
            }
            auto [type, name] = type_and_name(base, part);
            layouts_.push_back({std::move(part), std::move(type), std::move(name)});
        }
    }
}

std::optional<LayoutMatch> LayoutMatcher::match(const Package& source,
                                                const std::string& layout) const {
    if (layouts_.empty()) {
        return std::nullopt;
    }
    const auto source_layout = type_and_name(source, layout);
    const std::string& type = source_layout.first;
    const std::string& name = source_layout.second;
    const auto found = [&name](const Layout& l, LayoutMatchKind kind) {
        return LayoutMatch{l.part, kind, name, l.name};
    };
    if (type != "cust") {
        for (const auto& l : layouts_) {
            if (l.type == type) {
                return found(l, LayoutMatchKind::type);
            }
        }
    }
    if (!name.empty()) {
        for (const auto& l : layouts_) {
            if (l.name == name) {
                return found(l, LayoutMatchKind::name);
            }
        }
    }
    const auto obj = std::find_if(
        layouts_.begin(), layouts_.end(), [](const Layout& l) { return l.type == "obj"; });
    return found(obj != layouts_.end() ? *obj : layouts_.front(), LayoutMatchKind::fallback);
}

}  // namespace pptctl::detail
