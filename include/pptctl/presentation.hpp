// pptctl/presentation.hpp
//
// Document layer: semantic model of a presentation (slides, masters, layouts).
// Sits on top of the package layer.

#pragma once

#include <pptctl/package.hpp>
#include <cstddef>
#include <cstdint>
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

class Presentation {
   public:
    explicit Presentation(Package& package);

    // Load presentation metadata. Returns false if the package isn't a valid pptx.
    [[nodiscard]] bool load();

    [[nodiscard]] std::size_t slide_count() const noexcept { return slide_count_; }
    [[nodiscard]] CanvasSize canvas() const noexcept { return canvas_; }
    [[nodiscard]] std::size_t master_count() const noexcept { return master_count_; }
    [[nodiscard]] std::size_t layout_count() const noexcept { return layout_count_; }
    [[nodiscard]] const std::vector<std::string>& fonts() const noexcept { return fonts_; }
    [[nodiscard]] const MediaSummary& media() const noexcept { return media_; }

   private:
    Package* package_;
    std::size_t slide_count_ = 0;
    CanvasSize canvas_{};
    std::size_t master_count_ = 0;
    std::size_t layout_count_ = 0;
    std::vector<std::string> fonts_;
    MediaSummary media_{};
};

}  // namespace pptctl
