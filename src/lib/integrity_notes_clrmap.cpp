// integrity_notes_clrmap.cpp — the "notes-clrmap" rule: speaker-notes slides
// without a colour-map override, which Keynote refuses (#166).

#include <algorithm>
#include <cstddef>
#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <pugixml.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kNotesSlideType =
    "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";

std::string_view local_name(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

}  // namespace

bool notes_lack_clr_map_ovr(std::span<const std::byte> xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8)) {
        return false;
    }
    const auto root = doc.document_element();
    if (local_name(root) != "notes") {
        return false;
    }
    const auto children = root.children();
    return std::none_of(children.begin(), children.end(), [](const pugi::xml_node& child) {
        return child.type() == pugi::node_element && local_name(child) == "clrMapOvr";
    });
}

// The schema makes p:clrMapOvr optional in p:notes, and PowerPoint and
// LibreOffice open notes slides without it, but Keynote refuses the whole
// deck, with no message. LibreOffice and ONLYOFFICE write notes slides this
// way. Slides and layouts without it open fine, so only notes are checked.
void integrity_notes_clrmap(const Package& pkg,
                            const PartGraph& /*graph*/,
                            const LintOptions& /*opts*/,
                            std::vector<Diagnostic>& out) {
    for (const auto& part : pkg.part_names()) {
        if (pkg.content_type(part) != kNotesSlideType ||
            !notes_lack_clr_map_ovr(pkg.read_part(part))) {
            continue;
        }
        out.push_back({"notes-clrmap",
                       Severity::Error,
                       0,
                       "",
                       "notes slide has no p:clrMapOvr: Keynote refuses to open the deck "
                       "(PowerPoint writes <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>)",
                       part});
    }
}

}  // namespace pptctl::detail
