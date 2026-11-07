// merge_charts.cpp — find imported charts Google Slides will draw in another
// theme (#167).
//
// PowerPoint, Keynote and LibreOffice draw a chart in its own slide's theme;
// Google Slides draws every chart in the presentation's first theme: series
// colours the chart leaves to the theme (accent1…) and its minor font. merge
// reports these charts rather than rewriting them.

#include <cstddef>
#include <pptctl/package.hpp>
#include <pugixml.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kChartExRel =
    "http://schemas.microsoft.com/office/2014/relationships/chartEx";

// The target of `source`'s first internal relationship of short type `type`.
std::string target_of(const Package& pkg, std::string_view source, std::string_view type) {
    const auto full = std::string(kRelNs) + std::string(type);
    for (const auto& rel : pkg.relationships(source)) {
        if (!rel.external && rel.type == full) {
            return rel.target;
        }
    }
    return {};
}

std::string_view local_name(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node child_named(const pugi::xml_node& parent, std::string_view local) {
    return parent.find_child([&](const pugi::xml_node& n) { return local_name(n) == local; });
}

// The theme of `slide`, through its layout and master; "" if a link is missing.
std::string slide_theme(const Package& pkg, const std::string& slide) {
    const auto layout = target_of(pkg, slide, "slideLayout");
    const auto master = layout.empty() ? layout : target_of(pkg, layout, "slideMaster");
    return master.empty() ? master : target_of(pkg, master, "theme");
}

}  // namespace

std::string theme_look(const Package& pkg, const std::string& theme) {
    if (theme.empty()) {
        return {};
    }
    const auto bytes = pkg.read_part(theme);
    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
        return {};
    }
    const auto elements = child_named(doc.document_element(), "themeElements");
    std::string look;
    // Each scheme colour (dk1, lt1, …, accent1…6, hlink, folHlink) as written:
    // an srgbClr value, or a sysClr's last computed value.
    for (const auto& slot : child_named(elements, "clrScheme").children()) {
        const auto colour = slot.first_child();
        look += std::string(local_name(slot)) + "=" + colour.attribute("val").as_string("") + "/" +
                colour.attribute("lastClr").as_string("") + ";";
    }
    const auto fonts = child_named(elements, "fontScheme");
    for (const auto* which : {"majorFont", "minorFont"}) {
        const auto latin = child_named(child_named(fonts, which), "latin");
        look += std::string(which) + "=" + latin.attribute("typeface").as_string("") + ";";
    }
    return look;
}

std::string first_theme_look(const Package& pkg) {
    const auto pres = target_of(pkg, "", "officeDocument");
    const auto bytes = pkg.read_part(pres);
    pugi::xml_document doc;
    if (pres.empty() ||
        !doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
        return {};
    }
    const auto list = child_named(doc.document_element(), "sldMasterIdLst");
    const auto first =
        list.find_child([](const pugi::xml_node& n) { return local_name(n) == "sldMasterId"; });
    for (const auto& attr : first.attributes()) {
        const std::string_view name = attr.name();
        if (name.ends_with(":id") && name != "id") {
            const auto master = pkg.resolve(pres, attr.value());
            return master.empty() ? master : theme_look(pkg, target_of(pkg, master, "theme"));
        }
    }
    return {};
}

std::size_t charts_off_theme(const Package& source,
                             std::span<const std::string> slides,
                             const std::string& first_look) {
    std::set<std::string> charts;
    for (const auto& slide : slides) {
        const auto look = theme_look(source, slide_theme(source, slide));
        if (look.empty() || first_look.empty() || look == first_look) {
            continue;
        }
        for (const auto& rel : source.relationships(slide)) {
            if (!rel.external &&
                (rel.type == std::string(kRelNs) + "chart" || rel.type == kChartExRel)) {
                charts.insert(rel.target);
            }
        }
    }
    return charts.size();
}

}  // namespace pptctl::detail
