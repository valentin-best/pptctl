// pptctl/presentation.cpp — reads presentation.xml, counts slides/masters/layouts,
// extracts canvas dimensions, fonts referenced in the theme, and media summary.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <pptctl/presentation.hpp>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <string_view>

namespace pptctl {

namespace {

// Parse XML from a part's bytes. Returns true if the doc was loaded.
bool load_xml_part(const Package& pkg, std::string_view name, pugi::xml_document& doc) {
    const auto bytes = pkg.read_part(name);
    if (bytes.empty()) {
        return false;
    }
    const auto result =
        doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    return static_cast<bool>(result);
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool is_image(std::string_view name) {
    return ends_with(name, ".png") || ends_with(name, ".jpg") || ends_with(name, ".jpeg") ||
           ends_with(name, ".gif") || ends_with(name, ".bmp") || ends_with(name, ".tiff") ||
           ends_with(name, ".webp") || ends_with(name, ".svg") || ends_with(name, ".emf") ||
           ends_with(name, ".wmf");
}

bool is_video(std::string_view name) {
    return ends_with(name, ".mp4") || ends_with(name, ".mov") || ends_with(name, ".avi") ||
           ends_with(name, ".wmv") || ends_with(name, ".mkv") || ends_with(name, ".webm");
}

bool is_audio(std::string_view name) {
    return ends_with(name, ".mp3") || ends_with(name, ".wav") || ends_with(name, ".m4a") ||
           ends_with(name, ".ogg") || ends_with(name, ".aac") || ends_with(name, ".flac");
}

// Read entry size for a single part (cheap; doesn't decompress).
// We approximate by reading the part — it's not free but fine for v0.
std::size_t part_size(const Package& pkg, std::string_view name) {
    return pkg.read_part(name).size();
}

std::string to_upper_hex(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// Resolve one a:clrScheme slot to an RRGGBB hex value. A slot holds either an
// explicit a:srgbClr (@val) or a system color a:sysClr (whose @lastClr carries
// the resolved RGB). Returns "" if the slot has neither.
std::string resolve_scheme_color(pugi::xml_node slot) {
    if (auto srgb = slot.child("a:srgbClr")) {
        return to_upper_hex(srgb.attribute("val").as_string(""));
    }
    if (auto sys = slot.child("a:sysClr")) {
        return to_upper_hex(sys.attribute("lastClr").as_string(""));
    }
    return {};
}

// Fill `out` from an a:clrScheme node. The 12 slots are fixed by the OOXML spec;
// we emit them in canonical order, skipping any that don't resolve to a color.
void parse_color_scheme(pugi::xml_node clr_scheme, ColorScheme& out) {
    if (!clr_scheme) {
        return;
    }
    static constexpr std::array<const char*, 12> kSlots = {"a:dk1",
                                                           "a:lt1",
                                                           "a:dk2",
                                                           "a:lt2",
                                                           "a:accent1",
                                                           "a:accent2",
                                                           "a:accent3",
                                                           "a:accent4",
                                                           "a:accent5",
                                                           "a:accent6",
                                                           "a:hlink",
                                                           "a:folHlink"};
    out.name = clr_scheme.attribute("name").as_string("");
    for (const char* slot : kSlots) {
        auto node = clr_scheme.child(slot);
        if (!node) {
            continue;
        }
        std::string value = resolve_scheme_color(node);
        if (!value.empty()) {
            out.colors.push_back({std::string(slot).substr(2), std::move(value)});
        }
    }
}

}  // namespace

Presentation::Presentation(Package& package) : package_(&package) {}

bool Presentation::load() {
    if (!package_ || !package_->is_open()) {
        return false;
    }

    // --- presentation.xml: slide count + canvas dimensions ------------------
    pugi::xml_document pres_doc;
    if (!load_xml_part(*package_, "ppt/presentation.xml", pres_doc)) {
        return false;
    }
    auto pres = pres_doc.child("p:presentation");
    if (!pres) {
        return false;
    }
    auto sld_size = pres.child("p:sldSz");
    if (sld_size) {
        canvas_.width_emu = sld_size.attribute("cx").as_llong(0);
        canvas_.height_emu = sld_size.attribute("cy").as_llong(0);
    }
    auto sld_id_lst = pres.child("p:sldIdLst");
    if (sld_id_lst) {
        for (auto sld_id : sld_id_lst.children("p:sldId")) {
            ++slide_count_;
            const char* rid = sld_id.attribute("r:id").as_string("");
            if (rid[0] != '\0') {
                std::string target = package_->resolve("ppt/presentation.xml", rid);
                if (!target.empty()) {
                    slide_parts_.push_back(std::move(target));
                }
            }
        }
    }

    // --- enumerate parts: count masters, layouts, summarize media -----------
    const auto& parts = package_->part_names();
    std::set<std::string> theme_paths;
    for (const auto& name : parts) {
        if (starts_with(name, "ppt/slideMasters/slideMaster") && ends_with(name, ".xml")) {
            ++master_count_;
        } else if (starts_with(name, "ppt/slideLayouts/slideLayout") && ends_with(name, ".xml")) {
            ++layout_count_;
        } else if (starts_with(name, "ppt/theme/") && ends_with(name, ".xml")) {
            theme_paths.insert(name);
        } else if (starts_with(name, "ppt/media/")) {
            if (is_image(name)) {
                ++media_.image_count;
                media_.image_bytes += part_size(*package_, name);
            } else if (is_video(name)) {
                ++media_.video_count;
            } else if (is_audio(name)) {
                ++media_.audio_count;
            }
        }
    }

    // --- theme: collect font names + the primary color scheme ---------------
    std::set<std::string> font_set;
    for (const auto& theme_path : theme_paths) {
        pugi::xml_document theme_doc;
        if (!load_xml_part(*package_, theme_path, theme_doc)) {
            continue;
        }
        auto root = theme_doc.child("a:theme");
        auto elements = root.child("a:themeElements");

        // a:fontScheme/{a:majorFont,a:minorFont}/a:latin/@typeface — across all themes.
        auto scheme = elements.child("a:fontScheme");
        for (const char* slot : {"a:majorFont", "a:minorFont"}) {
            auto f = scheme.child(slot);
            auto latin = f.child("a:latin");
            if (latin) {
                const char* tf = latin.attribute("typeface").as_string("");
                if (tf && tf[0] != '\0') {
                    font_set.emplace(tf);
                }
            }
        }

        // a:clrScheme — surface the first theme that has one as the deck's scheme.
        if (color_scheme_.empty()) {
            parse_color_scheme(elements.child("a:clrScheme"), color_scheme_);
        }
    }
    fonts_.assign(font_set.begin(), font_set.end());

    return true;
}

}  // namespace pptctl
