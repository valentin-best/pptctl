// merge_notes.cpp — give notes slides the p:clrMapOvr Keynote needs (#166).

#include <cstddef>
#include <pugixml.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "integrity_internal.hpp"
#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kDrawingMlNs = "http://schemas.openxmlformats.org/drawingml/2006/main";

// Serialize without indentation, keeping the declaration it was parsed with.
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

// "p:" for <p:notes>, "" for an unprefixed root.
std::string prefix_of(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

// The prefix the root binds DrawingML to ("a"), or "" if it doesn't.
std::string drawingml_prefix(const pugi::xml_node& root) {
    for (const auto& attr : root.attributes()) {
        const std::string_view name = attr.name();
        if (name.starts_with("xmlns:") && attr.value() == kDrawingMlNs) {
            return std::string(name.substr(6));
        }
    }
    return {};
}

// Add <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr> to a notes slide that
// lacks it, before a trailing p:extLst as the schema orders them. Empty if the
// part already has it or doesn't parse.
std::vector<std::byte> with_clr_map_ovr(const std::vector<std::byte>& xml) {
    if (!notes_lack_clr_map_ovr(xml)) {
        return {};
    }
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), flags, pugi::encoding_utf8)) {
        return {};
    }
    auto root = doc.document_element();
    const auto p = prefix_of(root);
    pugi::xml_node ext_lst;
    for (const auto& child : root.children()) {
        if (child.type() == pugi::node_element && std::string_view(child.name()) == p + "extLst") {
            ext_lst = child;
        }
    }
    const std::string name = p + "clrMapOvr";
    auto ovr =
        ext_lst ? root.insert_child_before(name.c_str(), ext_lst) : root.append_child(name.c_str());
    const auto a = drawingml_prefix(root);
    auto mapping = ovr.append_child(((a.empty() ? "a" : a) + ":masterClrMapping").c_str());
    if (a.empty()) {
        mapping.append_attribute("xmlns:a") = std::string(kDrawingMlNs).c_str();
    }
    return save_xml(doc);
}

}  // namespace

std::size_t add_notes_clr_map_ovr(MergeTarget& target, std::span<const std::string> notes) {
    std::size_t added = 0;
    for (const auto& part : notes) {
        auto fixed = with_clr_map_ovr(target.read_part(part));
        if (!fixed.empty()) {
            target.replace_part(part, std::move(fixed));
            ++added;
        }
    }
    return added;
}

}  // namespace pptctl::detail
