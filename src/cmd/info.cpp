// pptctl info — prints metadata about a .pptx file.

#include <cstddef>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::cmd {

namespace {

std::string join(const std::vector<std::string>& xs, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i != 0U) {
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
        pptctl::eprintln("error: could not open {}{}",
                         path,
                         pkg.error().empty() ? std::string() : ": " + pkg.error());
        return 1;
    }

    pptctl::Presentation pres(pkg);
    if (!pres.load()) {
        pptctl::eprintln("error: {} is not a valid .pptx (no ppt/presentation.xml)", path);
        return 1;
    }

    const auto file_size =
        std::filesystem::exists(path) ? std::filesystem::file_size(path) : std::size_t{0};
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
        const auto& scheme = pres.color_scheme();
        out["theme_color_scheme"] = scheme.name;
        auto colors = nlohmann::json::object();
        for (const auto& c : scheme.colors) {
            colors[c.name] = c.value;
        }
        out["theme_colors"] = colors;
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
        pptctl::println("{:<{}}{:.2f}in × {:.2f}in",
                        "canvas",
                        kLabelW,
                        canvas.width_inches(),
                        canvas.height_inches());
        pptctl::println("{:<{}}{}", "masters", kLabelW, pres.master_count());
        pptctl::println("{:<{}}{}", "layouts", kLabelW, pres.layout_count());
        if (!pres.fonts().empty()) {
            pptctl::println("{:<{}}{}", "fonts referenced", kLabelW, join(pres.fonts(), ", "));
        }
        const auto& scheme = pres.color_scheme();
        if (!scheme.colors.empty()) {
            std::vector<std::string> swatches;
            swatches.reserve(scheme.colors.size());
            for (const auto& c : scheme.colors) {
                swatches.push_back(std::format("{} #{}", c.name, c.value));
            }
            const std::string value =
                scheme.name.empty() ? join(swatches, ", ")
                                    : std::format("{} — {}", scheme.name, join(swatches, ", "));
            pptctl::println("{:<{}}{}", "theme colors", kLabelW, value);
        }
        pptctl::println("{:<{}}{} images ({}), {} video, {} audio",
                        "media",
                        kLabelW,
                        media.image_count,
                        human_bytes(media.image_bytes),
                        media.video_count,
                        media.audio_count);
        pptctl::println("{:<{}}{}", "parts in package", kLabelW, pkg.part_count());
    }
    return 0;
}

}  // namespace pptctl::cmd
