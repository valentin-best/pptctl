// optimize_fonts.cpp — the "fonts" pass: embedded fonts (#101).
//
// PowerPoint embeds fonts as parts (ppt/fonts/fontN.fntdata) referenced only
// from ppt/presentation.xml: each p:embeddedFontLst/p:embeddedFont names a
// typeface and the r:ids of up to four styles (p:regular, p:bold, p:italic,
// p:boldItalic). The pass removes that list and lists every font part it names
// as removed; complete_removals then drops their presentation relationships
// and any content-type Overrides. The fntdata Default in [Content_Types].xml
// stays: a Default for an extension no part uses is valid, and leaving it
// avoids rewriting a part for nothing.
//
// It also drops the embedTrueTypeFonts attribute of p:presentation (the "Embed
// fonts in the file" option). Left on, PowerPoint re-embeds every font it has
// installed on the next save and undoes the pass. saveSubsetFonts stays: Office
// writes it on decks that embed nothing, and it means nothing without
// embedTrueTypeFonts.
//
// Only ppt/presentation.xml is re-serialized, raw and with its declaration,
// the way package.cpp rewrites .rels and [Content_Types].xml.

#include <array>
#include <cstddef>
#include <optional>
#include <pugixml.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "optimize_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kPresentation = "ppt/presentation.xml";

// The style elements of a p:embeddedFont and the suffix each adds to the
// typeface in the report ("Lato" + " Bold").
struct FontStyle {
    const char* element;
    std::string_view suffix;
};
constexpr std::array<FontStyle, 4> kStyles{{
    {"p:regular", ""},
    {"p:bold", " Bold"},
    {"p:italic", " Italic"},
    {"p:boldItalic", " Bold Italic"},
}};

// Parse an XML part keeping its declaration, so re-serializing preserves it.
bool load_xml(pugi::xml_document& doc, std::span<const std::byte> xml) {
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    return static_cast<bool>(doc.load_buffer(xml.data(), xml.size(), flags, pugi::encoding_utf8));
}

// Serialize without indentation or added newlines, the way Office writes parts.
std::vector<std::byte> save_xml(const pugi::xml_document& doc) {
    struct BytesWriter : pugi::xml_writer {
        std::vector<std::byte> out;
        void write(const void* data, std::size_t size) override {
            const auto* p = static_cast<const std::byte*>(data);
            out.insert(out.end(), p, p + size);
        }
    } writer;
    doc.save(writer, "", pugi::format_raw, pugi::encoding_utf8);
    return std::move(writer.out);
}

// The report name of one embedded font style: "Lato Bold".
std::string font_name(std::string_view typeface, std::string_view suffix) {
    return std::string(typeface.empty() ? "(unnamed)" : typeface) + std::string(suffix);
}

}  // namespace

PassResult optimize_fonts(const Package& pkg,
                          const PartGraph& /*graph*/,
                          const OptimizeOptions& /*opts*/) {
    PassResult result;
    pugi::xml_document doc;
    if (!load_xml(doc, pkg.read_part(kPresentation))) {
        return result;
    }
    auto presentation = doc.child("p:presentation");
    const auto list = presentation.child("p:embeddedFontLst");
    if (!list.child("p:embeddedFont")) {
        return result;  // nothing embedded: no changes at all
    }

    for (const auto& font : list.children("p:embeddedFont")) {
        const std::string_view typeface = font.child("p:font").attribute("typeface").as_string("");
        for (const auto& style : kStyles) {
            const std::string_view rid = font.child(style.element).attribute("r:id").as_string("");
            if (rid.empty()) {
                continue;
            }
            auto part = pkg.resolve(kPresentation, rid);
            const auto size = part.empty() ? std::nullopt : pkg.part_size(part);
            // A dangling r:id has no part to remove; a part named twice is reported once.
            if (!size || result.changes.removed.contains(part)) {
                continue;
            }
            result.changes.removed.insert(part);
            result.actions.push_back(
                {std::move(part), "removed", font_name(typeface, style.suffix), *size, 0});
        }
    }

    presentation.remove_child(list);
    presentation.remove_attribute("embedTrueTypeFonts");
    result.changes.replaced[std::string(kPresentation)] = save_xml(doc);
    return result;
}

}  // namespace pptctl::detail
