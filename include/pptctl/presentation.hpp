// pptctl/presentation.hpp
//
// Document layer: semantic model of a presentation (slides, masters, layouts).
// Sits on top of the package layer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <pptctl/package.hpp>
#include <string>
#include <vector>

namespace pptctl {

// EMU = English Metric Units. 914400 EMU per inch. Native unit of OOXML.
using Emu = std::int64_t;

constexpr Emu kEmuPerInch = 914400;

struct CanvasSize {
    Emu width_emu = 0;
    Emu height_emu = 0;

    [[nodiscard]] double width_inches() const noexcept {
        return static_cast<double>(width_emu) / static_cast<double>(kEmuPerInch);
    }
    [[nodiscard]] double height_inches() const noexcept {
        return static_cast<double>(height_emu) / static_cast<double>(kEmuPerInch);
    }
};

struct MediaSummary {
    std::size_t image_count = 0;
    std::size_t video_count = 0;
    std::size_t audio_count = 0;
    std::size_t image_bytes = 0;
};

// One slot of a theme's color scheme (a:clrScheme), e.g. {"accent1", "4F81BD"}.
struct ThemeColor {
    std::string name;   // slot name without the "a:" prefix: dk1, lt1, accent1, …
    std::string value;  // 6-digit RRGGBB hex, uppercase, no leading '#'
};

// A theme's color scheme: the scheme name plus its ordered color slots.
struct ColorScheme {
    std::string name;                // a:clrScheme/@name, e.g. "Office"
    std::vector<ThemeColor> colors;  // in OOXML slot order

    [[nodiscard]] bool empty() const noexcept {
        return colors.empty();
    }
};

class Presentation {
 public:
    explicit Presentation(Package& package);

    // Load presentation metadata. Returns false if the package isn't a valid pptx.
    [[nodiscard]] bool load();

    [[nodiscard]] std::size_t slide_count() const noexcept {
        return slide_count_;
    }
    [[nodiscard]] const std::vector<std::string>& slide_parts() const noexcept {
        return slide_parts_;
    }
    [[nodiscard]] CanvasSize canvas() const noexcept {
        return canvas_;
    }
    [[nodiscard]] std::size_t master_count() const noexcept {
        return master_count_;
    }
    [[nodiscard]] std::size_t layout_count() const noexcept {
        return layout_count_;
    }
    [[nodiscard]] const std::vector<std::string>& fonts() const noexcept {
        return fonts_;
    }
    [[nodiscard]] const ColorScheme& color_scheme() const noexcept {
        return color_scheme_;
    }
    [[nodiscard]] const MediaSummary& media() const noexcept {
        return media_;
    }

 private:
    Package* package_;
    std::size_t slide_count_ = 0;
    std::vector<std::string> slide_parts_;
    CanvasSize canvas_{};
    std::size_t master_count_ = 0;
    std::size_t layout_count_ = 0;
    std::vector<std::string> fonts_;
    ColorScheme color_scheme_{};
    MediaSummary media_{};
};

}  // namespace pptctl
