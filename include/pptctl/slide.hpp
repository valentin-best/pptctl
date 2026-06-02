// pptctl/slide.hpp
//
// Document layer: the content of a single slide — its shape tree, text
// frames, paragraphs, and runs. Sits on top of the package layer, alongside
// presentation.hpp. Read-only: this layer never serializes slides back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>  // for Emu
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

enum class ShapeKind : std::uint8_t {
    Placeholder,
    TextBox,
    Picture,
    Table,
    Chart,
    Diagram,
    Ole,
    Group,
    Connector,
    Other,
};

// Run-level formatting from a:rPr. Each field is set only when the source
// attribute is present, so "unset" (inherit) stays distinct from "explicitly
// off".
struct RunProperties {
    std::optional<bool> bold;         // a:rPr@b
    std::optional<bool> italic;       // a:rPr@i
    std::optional<bool> underlined;   // a:rPr@u != "none"
    std::optional<int> size_centipt;  // a:rPr@sz, hundredths of a point (native)
    std::optional<std::string> font;  // a:rPr/a:latin@typeface

    // Convenience; mirrors the EMU->inches pattern. 0 when size is unset.
    [[nodiscard]] double size_pt() const noexcept {
        return size_centipt ? static_cast<double>(*size_centipt) / 100.0 : 0.0;
    }
};

struct Run {
    std::string text;
    RunProperties props;
    bool is_field = false;  // came from a:fld rather than a:r
};

struct Paragraph {
    int level = 0;  // a:pPr@lvl (bullet / indent depth)
    std::vector<Run> runs;

    [[nodiscard]] std::string text() const {
        std::string out;
        for (const auto& r : runs) {
            out += r.text;
        }
        return out;
    }
};

// Present only when the shape carries an a:xfrm. EMU, the native OOXML unit.
struct Geometry {
    Emu off_x = 0;
    Emu off_y = 0;
    Emu ext_cx = 0;
    Emu ext_cy = 0;
};

// A picture's blip reference and the media part it resolves to. Present only on
// shapes with kind == Picture. parse() fills rel_id/linked/alt_text from the
// slide XML; Slide::load() fills target/content_type/size_bytes via the
// package's relationship graph (left empty/nullopt for linked or unresolved
// images).
struct Picture {
    std::string rel_id;                       // a:blip@r:embed, or @r:link when linked
    bool linked = false;                      // from r:link (external) rather than r:embed
    std::string target;                       // resolved media part; empty if linked/unresolved
    std::string content_type;                 // e.g. "image/png"; empty if unknown
    std::optional<std::uint64_t> size_bytes;  // uncompressed media size; nullopt if unknown
    std::optional<std::string> alt_text;      // p:cNvPr@descr
};

struct Shape {
    ShapeKind kind = ShapeKind::Other;
    std::string id;                               // p:cNvPr@id
    std::string name;                             // p:cNvPr@name
    std::optional<std::string> placeholder_type;  // p:nvPr/p:ph@type
    std::optional<Geometry> geometry;
    std::vector<Paragraph> paragraphs;  // text body; empty for non-text shapes
    std::vector<Shape> children;        // populated only for Group, recursive
    std::optional<Picture> picture;     // populated only when kind == Picture
};

class Slide {
 public:
    Slide() = default;

    // Read and parse a slide part from the package. Returns false if the part
    // is missing or not parseable.
    [[nodiscard]] bool load(const Package& pkg, std::string_view part_name);

    // Parse slide XML directly from bytes. Exposed for testing the parser
    // without a package. Returns false if the XML can't be parsed.
    [[nodiscard]] bool parse(std::string_view xml);

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }
    [[nodiscard]] const std::string& part_name() const noexcept {
        return part_name_;
    }
    [[nodiscard]] const std::vector<Shape>& shapes() const noexcept {
        return shapes_;
    }

    void set_index(std::size_t i) noexcept {
        index_ = i;
    }

 private:
    std::size_t index_ = 0;
    std::string part_name_;
    std::vector<Shape> shapes_;
};

}  // namespace pptctl
