// pptctl info — prints metadata about a .pptx file.

#include <nlohmann/json.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::cmd {

namespace {

std::string human_bytes(std::size_t n) {
    constexpr double kKi = 1024.0;
    double v = static_cast<double>(n);
    const char* unit = "B";
    if (v >= kKi) { v /= kKi; unit = "KiB"; }
    if (v >= kKi) { v /= kKi; unit = "MiB"; }
    if (v >= kKi) { v /= kKi; unit = "GiB"; }
    if (n < 1024) {
        return std::format("{} {}", n, unit);
    }
    return std::format("{:.1f} {}", v, unit);
}

std::string join(const std::vector<std::string>& xs, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) {
            out.append(sep);
        }
        out.append(xs[i]);
    }
    return out;
}

}  // namespace

int info(const std::string& path, bool json) {
    pptctl::Package pkg;
    if (!pkg.open(path)) {
        pptctl::eprintln("error: could not open {}", path);
        return 1;
    }

    pptctl::Presentation pres(pkg);
    if (!pres.load()) {
        pptctl::eprintln("error: {} is not a valid .pptx (no ppt/presentation.xml)", path);
        return 1;
    }

    const auto file_size = std::filesystem::exists(path)
                               ? std::filesystem::file_size(path)
                               : std::size_t{0};
    const auto canvas = pres.canvas();
    const auto media = pres.media();

    if (json) {
        nlohmann::json out;
        out["file"] = path;
        out["file_size_bytes"] = file_size;
        out["slides"] = pres.slide_count();
        out["canvas"] = {
            {"width_emu", canvas.width_emu},
            {"height_emu", canvas.height_emu},
            {"width_inches", canvas.width_inches()},
            {"height_inches", canvas.height_inches()},
        };
        out["masters"] = pres.master_count();
        out["layouts"] = pres.layout_count();
        out["fonts"] = pres.fonts();
        out["media"] = {
            {"images", media.image_count},
            {"images_bytes", media.image_bytes},
            {"videos", media.video_count},
            {"audios", media.audio_count},
        };
        out["parts_in_package"] = pkg.part_count();
        pptctl::println("{}", out.dump(2));
    } else {
        constexpr int kLabelW = 20;
        pptctl::println("{:<{}}{}  ({})", "file", kLabelW, path, human_bytes(file_size));
        pptctl::println("{:<{}}{}", "slides", kLabelW, pres.slide_count());
        pptctl::println("{:<{}}{:.2f}in × {:.2f}in", "canvas", kLabelW,
                        canvas.width_inches(), canvas.height_inches());
        pptctl::println("{:<{}}{}", "masters", kLabelW, pres.master_count());
        pptctl::println("{:<{}}{}", "layouts", kLabelW, pres.layout_count());
        if (!pres.fonts().empty()) {
            pptctl::println("{:<{}}{}", "fonts referenced", kLabelW, join(pres.fonts(), ", "));
        }
        pptctl::println("{:<{}}{} images ({}), {} video, {} audio", "media", kLabelW,
                        media.image_count, human_bytes(media.image_bytes),
                        media.video_count, media.audio_count);
        pptctl::println("{:<{}}{}", "parts in package", kLabelW, pkg.part_count());
    }
    return 0;
}

}  // namespace pptctl::cmd
