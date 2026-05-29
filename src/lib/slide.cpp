// pptctl/slide.cpp — parses a slide's shape tree into a content model.

#include <pptctl/slide.hpp>
#include <pugixml.hpp>

namespace pptctl {

bool Slide::parse(std::string_view xml) {
    shapes_.clear();
    pugi::xml_document doc;
    const auto result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    return static_cast<bool>(result);
}

bool Slide::load(const Package& pkg, std::string_view part_name) {
    part_name_ = std::string(part_name);
    const auto bytes = pkg.read_part(part_name);
    if (bytes.empty()) {
        return false;
    }
    return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}  // namespace pptctl
