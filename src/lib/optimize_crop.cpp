// optimize_crop.cpp — the "crop" pass: cropped-out image regions (#104).
//
// PowerPoint keeps the whole image behind a crop: the a:srcRect in a
// picture's a:blipFill only says which part of it to show, so a 10 % sliver
// still ships the full photo. For each reachable PNG/JPEG media part whose
// every use carries the same srcRect, this pass cuts the pixels outside it and
// rewrites that srcRect in each referencing part to match the smaller image.
// The frame on the slide never changes.
//
// A use is an a:blip r:embed in a stretched a:blipFill, in any part that draws
// (slide, layout, master, notes, chart, ...): a picture, a shape's picture
// fill, a background, a table cell. srcRect maps the image into the frame the
// same way for all of them. The media is left alone, and reported as skipped
// when any of its uses is cropped, if
//   * its uses differ in their crop (splitting the media per crop could come
//     later; YAGNI for now), or one of them is not a use the pass can rewrite:
//     a tiled fill (draws the image at its own pixel size), a blip with a
//     companion image the same srcRect also crops (an SVG original, an
//     artistic-effect layer), a reference that isn't an a:blip r:embed, a
//     non-image relationship, a relationship the part's XML never names, or
//     a part that won't parse;
//   * it is a JPEG with a non-identity EXIF orientation: whether a crop
//     applies before or after the rotation is up to the renderer;
//   * the codec refuses it (16-bit PNG, ...).
//
// The kept pixels are the smallest whole-pixel rectangle around the visible
// region, and srcRect is rewritten to the remainder, so the frame shows
// exactly the same image region as before. Rounding the edges to the nearest
// pixel instead would shift them by up to half a source pixel, which shows on
// sharp content shown enlarged (screenshots). When the crop already falls on
// pixel boundaries, as it mostly does, srcRect goes away. PNGs are decoded,
// cropped and re-encoded losslessly. JPEGs are cropped in the DCT domain
// (crop_jpeg), so no quality is lost, but their left and top edges stay on
// the 8/16 px block grid: srcRect keeps that remainder of up to 15 pixels,
// which the images pass then accounts for. Negative insets (an image shown
// with padding) have no pixels to cut; they stay, rescaled to the smaller
// image. As in the images pass, a crop is only kept when it saves at least
// kMinSavingsBytes.
//
// Media part names and content types never change. Only the parts whose
// srcRect changes are re-serialized, keeping their declaration, whitespace
// text (a run of " " is content) and comments.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <pptctl/image.hpp>
#include <pugixml.hpp>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "optimize_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr double kPercent = 100000.0;  // DrawingML percentages: 100000 = 100 %
constexpr std::uint64_t kMinSavingsBytes = 1024;
constexpr std::string_view kImageRelType = "/relationships/image";  // Transitional and Strict

std::string_view local_name(std::string_view name) {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool is(const pugi::xml_node& node, std::string_view local) {
    return local_name(node.name()) == local;
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view local) {
    for (const auto& c : node.children()) {
        if (c.type() == pugi::node_element && is(c, local)) {
            return c;
        }
    }
    return {};
}

// Parse a part for editing: keep its declaration, comments and PIs, and
// whitespace-only text, which in a:t is slide content.
bool load_xml(pugi::xml_document& doc, std::span<const std::byte> xml) {
    constexpr unsigned flags = pugi::parse_default | pugi::parse_declaration |
                               pugi::parse_comments | pugi::parse_pi | pugi::parse_ws_pcdata;
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

// Where a relationship id appears in a part: the element and the prefixed
// attribute naming it (r:embed, r:id, ...).
struct RefSite {
    pugi::xml_node element;
    pugi::xml_attribute attribute;
};

// A part that references media, parsed once, with its relationship-id uses.
struct Source {
    pugi::xml_document doc;
    std::map<std::string, std::vector<RefSite>, std::less<>> sites;
    bool dirty = false;
};

class Sources {
 public:
    explicit Sources(const Package& pkg) : pkg_(pkg) {}

    // The parsed part, or nullptr if it doesn't parse.
    Source* get(const std::string& part) {
        auto it = parts_.find(part);
        if (it == parts_.end()) {
            auto source = std::make_unique<Source>();
            if (load_xml(source->doc, pkg_.read_part(part))) {
                index(*source);
            } else {
                source.reset();
            }
            it = parts_.emplace(part, std::move(source)).first;
        }
        return it->second.get();
    }

    // The edited parts, re-serialized.
    void save_dirty(PartChanges& changes) const {
        for (const auto& [part, source] : parts_) {
            if (source && source->dirty) {
                changes.replaced[part] = save_xml(source->doc);
            }
        }
    }

 private:
    static void index(Source& source) {
        for (const auto& node : source.doc.select_nodes("//*")) {
            const auto el = node.node();
            for (const auto& a : el.attributes()) {
                const std::string_view name = a.name();
                if (name.find(':') != std::string_view::npos && !name.starts_with("xmlns:")) {
                    source.sites[a.value()].push_back({el, a});
                }
            }
        }
    }

    const Package& pkg_;
    std::map<std::string, std::unique_ptr<Source>> parts_;
};

// srcRect insets, in 1/1000 % of the image's width or height.
struct Insets {
    std::int64_t l = 0;
    std::int64_t t = 0;
    std::int64_t r = 0;
    std::int64_t b = 0;

    bool operator==(const Insets&) const = default;
    [[nodiscard]] bool none() const {
        return l == 0 && t == 0 && r == 0 && b == 0;
    }
};

std::optional<std::int64_t> read_inset(const pugi::xml_node& rect, const char* name) {
    const std::string_view v = rect.attribute(name).as_string("");
    std::int64_t out = 0;
    if (v.empty()) {
        return out;
    }
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{} || end != v.data() + v.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<Insets> read_insets(const pugi::xml_node& fill) {
    const auto rect = child(fill, "srcRect");
    const auto l = read_inset(rect, "l");
    const auto t = read_inset(rect, "t");
    const auto r = read_inset(rect, "r");
    const auto b = read_inset(rect, "b");
    if (!l || !t || !r || !b) {
        return std::nullopt;
    }
    return Insets{*l, *t, *r, *b};
}

void write_insets(pugi::xml_node fill, const Insets& insets) {
    auto rect = child(fill, "srcRect");
    if (insets.none()) {
        fill.remove_child(rect);
        return;
    }
    const std::array<std::pair<const char*, std::int64_t>, 4> sides{
        {{"l", insets.l}, {"t", insets.t}, {"r", insets.r}, {"b", insets.b}}};
    for (const auto& [name, value] : sides) {
        if (value == 0) {
            rect.remove_attribute(name);
        } else {
            auto a = rect.attribute(name);
            (a ? a : rect.append_attribute(name)).set_value(std::to_string(value).c_str());
        }
    }
}

// A blip with another image hanging off it (asvg:svgBlip, a14:imgLayer, ...):
// a descendant naming a relationship.
bool has_companion(const pugi::xml_node& blip) {
    for (const auto& node : blip.select_nodes(".//*")) {
        for (const auto& a : node.node().attributes()) {
            const auto local = local_name(a.name());
            if (std::string_view(a.name()).find(':') != std::string_view::npos &&
                (local == "embed" || local == "link")) {
                return true;
            }
        }
    }
    return false;
}

// An a:blipFill whose srcRect the pass would rewrite, and its current crop.
struct Fill {
    Source* source;
    pugi::xml_node node;
    Insets crop;
};

// One media part's uses: the fills the pass would rewrite, and why it can't
// when it can't.
struct Uses {
    std::vector<Fill> fills;
    std::string blocker;   // first reason the media can't be cropped
    bool cropped = false;  // some use we could read has a crop
    std::size_t count = 0;

    void block(std::string why) {
        if (blocker.empty()) {
            blocker = std::move(why);
        }
    }
};

Uses find_uses(const PartGraph& graph, const std::string& media, Sources& sources) {
    Uses uses;
    for (const auto& ref : graph.references_to(media)) {
        if (!ref.rel.type.ends_with(kImageRelType)) {
            uses.block("used through a non-image relationship");
            continue;
        }
        Source* source = ref.source.empty() ? nullptr : sources.get(ref.source);
        if (source == nullptr) {
            uses.block(std::format("{} can't be parsed", ref.source));
            continue;
        }
        const auto it = source->sites.find(ref.rel.id);
        if (it == source->sites.end()) {
            uses.block(std::format("a use in {} the pass can't see", ref.source));
            continue;
        }
        for (const auto& site : it->second) {
            ++uses.count;
            const auto fill = site.element.parent();
            if (!is(site.element, "blip") || local_name(site.attribute.name()) != "embed" ||
                !is(fill, "blipFill")) {
                uses.block("used outside a picture fill");
                continue;
            }
            const auto insets = read_insets(fill);
            uses.cropped = uses.cropped || !insets || !insets->none();
            if (!insets) {
                uses.block("unreadable crop");
            } else if (child(fill, "tile")) {
                uses.block("tiled fill");
            } else if (has_companion(site.element)) {
                uses.block("cropped together with an SVG or effect layer");
            } else {
                uses.fills.push_back({source, fill, *insets});
            }
        }
    }
    return uses;
}

std::optional<ImageFormat> media_format(const Package& pkg, const std::string& part) {
    const auto type = pkg.content_type(part);
    if (type == "image/png") {
        return ImageFormat::Png;
    }
    if (type == "image/jpeg" || type == "image/jpg" || type == "image/pjpeg") {
        return ImageFormat::Jpeg;
    }
    return std::nullopt;
}

// The visible region of a w x h image under `insets`, in pixels (fractional;
// beyond the image where an inset is negative).
struct Region {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
};

Region visible_region(const Insets& insets, double w, double h) {
    return {static_cast<double>(insets.l) / kPercent * w,
            static_cast<double>(insets.t) / kPercent * h,
            w - static_cast<double>(insets.r) / kPercent * w,
            h - static_cast<double>(insets.b) / kPercent * h};
}

// Whole-pixel edges enclosing a fractional one, within [0, max]. The slack
// keeps floating-point noise on an exact edge from adding a pixel.
std::uint32_t floor_px(double v, std::uint32_t max) {
    return static_cast<std::uint32_t>(
        std::clamp(std::floor(v + 1e-6), 0.0, static_cast<double>(max)));
}

std::uint32_t ceil_px(double v, std::uint32_t max) {
    return static_cast<std::uint32_t>(
        std::clamp(std::ceil(v - 1e-6), 0.0, static_cast<double>(max)));
}

// The insets that show `visible` out of the `kept` pixels. An edge that falls
// on a pixel boundary rounds to 0 (srcRect's unit is 1/100000 of the image).
Insets insets_within(const Region& visible, const PixelRect& kept) {
    const auto inset = [](double gap_px, std::uint32_t size) {
        return static_cast<std::int64_t>(std::llround(gap_px / size * kPercent));
    };
    return {inset(visible.left - kept.x, kept.width),
            inset(visible.top - kept.y, kept.height),
            inset(kept.x + kept.width - visible.right, kept.width),
            inset(kept.y + kept.height - visible.bottom, kept.height)};
}

Image crop_pixels(const Image& src, const PixelRect& rect) {
    Image out = src;
    out.width = rect.width;
    out.height = rect.height;
    out.pixels.resize(std::size_t{rect.width} * rect.height * src.channels);
    const std::size_t src_stride = std::size_t{src.width} * src.channels;
    const std::size_t row = std::size_t{rect.width} * src.channels;
    for (std::uint32_t y = 0; y < rect.height; ++y) {
        const auto from =
            src.pixels.begin() + static_cast<std::ptrdiff_t>((rect.y + y) * src_stride +
                                                             std::size_t{rect.x} * src.channels);
        std::copy_n(from, row, out.pixels.begin() + static_cast<std::ptrdiff_t>(y * row));
    }
    return out;
}

void skip(PassResult& result, const std::string& part, std::uint64_t size, std::string why) {
    result.actions.push_back({part, "skipped", std::move(why), size, size});
}

void process(const Package& pkg,
             const std::string& part,
             ImageFormat format,
             const Uses& uses,
             PassResult& result) {
    const auto bytes = pkg.read_part(part);
    const std::uint64_t before = bytes.size();
    if (sniff_image_format(bytes) != format) {
        return;  // content type and bytes disagree: not ours to touch
    }
    const auto dims = probe_image_dimensions(bytes);
    if (!dims) {
        return;
    }
    const Region visible = visible_region(uses.fills.front().crop, dims->width, dims->height);
    const std::uint32_t x0 = floor_px(visible.left, dims->width);
    const std::uint32_t y0 = floor_px(visible.top, dims->height);
    const std::uint32_t x1 = ceil_px(visible.right, dims->width);
    const std::uint32_t y1 = ceil_px(visible.bottom, dims->height);
    if (x1 <= x0 || y1 <= y0) {
        skip(result, part, before, "crop leaves no pixels");
        return;
    }
    if (x0 == 0 && y0 == 0 && x1 == dims->width && y1 == dims->height) {
        return;  // nothing outside the crop: padding only, or less than a pixel
    }
    const PixelRect want{x0, y0, x1 - x0, y1 - y0};

    std::vector<std::byte> encoded;
    PixelRect kept = want;
    if (format == ImageFormat::Jpeg) {
        auto cropped = crop_jpeg(bytes, want);
        if (cropped.bytes.empty()) {
            skip(result, part, before, cropped.reason);
            return;
        }
        if (cropped.exif_orientation >= 2) {
            skip(
                result, part, before, std::format("EXIF orientation {}", cropped.exif_orientation));
            return;
        }
        encoded = std::move(cropped.bytes);
        kept = cropped.kept;
    } else {
        const auto decoded = decode_png(bytes);
        if (!decoded.image) {
            skip(result, part, before, decoded.reason);
            return;
        }
        encoded = encode_png(crop_pixels(*decoded.image, want));
    }
    const auto after = static_cast<std::uint64_t>(encoded.size());
    if (encoded.empty() || after >= before || before - after < kMinSavingsBytes) {
        return;  // never grow, and don't rewrite a part for crumbs
    }

    const Insets remaining = insets_within(visible, kept);
    for (const auto& fill : uses.fills) {
        write_insets(fill.node, remaining);
        fill.source->dirty = true;
    }
    result.actions.push_back(
        {part,
         "cropped",
         std::format("{}x{} -> {}x{}", dims->width, dims->height, kept.width, kept.height),
         before,
         after});
    result.changes.replaced.emplace(part, std::move(encoded));
}

}  // namespace

PassResult optimize_crop(const Package& pkg,
                         const PartGraph& graph,
                         const OptimizeOptions& /*opts*/) {
    PassResult result;
    Sources sources(pkg);
    for (const auto& part : graph.reachable()) {
        const auto format = media_format(pkg, part);
        if (!format) {
            continue;
        }
        const Uses uses = find_uses(graph, part, sources);
        if (!uses.cropped) {
            continue;
        }
        if (uses.blocker.empty()) {
            const Insets& first = uses.fills.front().crop;
            const bool same = std::all_of(uses.fills.begin(), uses.fills.end(), [&](const Fill& f) {
                return f.crop == first;
            });
            if (!same) {
                skip(result,
                     part,
                     pkg.part_size(part).value_or(0),
                     std::format("{} uses with different crops", uses.count));
                continue;
            }
            process(pkg, part, *format, uses, result);
        } else {
            skip(result, part, pkg.part_size(part).value_or(0), uses.blocker);
        }
    }
    sources.save_dirty(result.changes);
    return result;
}

}  // namespace pptctl::detail
