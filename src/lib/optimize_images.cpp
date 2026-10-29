// optimize_images.cpp — the "images" pass: image recompression and downsampling (#103).
//
// For every reachable PNG/JPEG media part:
//
// 1. Size it. Every use found through the relationship graph contributes the
//    extent the image is displayed at, and the image needs enough pixels for
//    the largest one at the target density (2 x 96 DPI by default). On a
//    slide, a blip in a picture (p:pic) or a shape's picture fill (a:blipFill
//    in p:spPr) uses the shape's own a:ext, scaled through any enclosing
//    groups; a:srcRect crops are undone (the visible part must keep the
//    density, so the whole image needs proportionally more) and a:fillRect
//    insets applied. Everything else counts as the full canvas: uses from
//    layouts and masters, slide backgrounds, table cells, any reference that
//    isn't a plain blip, and parts other than slides. A tiled fill draws the
//    image at its own pixel size, so a tiled image is never resized. A
//    placeholder picture without its own a:xfrm is sized by the geometry it
//    inherits from its layout or master (PlaceholderInheritance, #93); one
//    with nothing to inherit is skipped and reported.
// 2. Decode it (the codec refuses CMYK JPEGs, 16-bit PNGs and the like, which
//    are skipped and reported), downsample by area averaging when it has more
//    pixels than needed, never upscaling, and re-encode in the same format:
//    JPEG at the configured quality, progressive, keeping its ICC profile and
//    EXIF orientation (sizing compares in stored orientation); PNG at maximum
//    deflate, alpha and colour chunks intact.
// 3. Keep the result only if it is smaller, by at least kMinSavingsBytes:
//    every rewritten part is churn (diff reports the picture as changed), so
//    a tiny icon is left alone for the sake of a few dozen bytes. A JPEG
//    re-encoded at the same size must also save kMinRecompressSavings, so an
//    already well-compressed photo isn't put through another lossy
//    generation for a sliver. When a downsampled image doesn't pass, the
//    image is re-encoded at its own size instead, under the same rules:
//    resampling blends a palette screenshot's colours, and the truecolour
//    result can be larger than the original that a lossless recompress
//    would still shrink.
//
// The part name, extension and content type never change, so no relationship
// or [Content_Types].xml edit is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <pptctl/image.hpp>
#include <pptctl/slide.hpp>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "optimize_internal.hpp"
#include "resample.hpp"

namespace pptctl::detail {

namespace {

constexpr double kEmuPerInch = 914400.0;
constexpr double kCanvasDpi = 96.0;
constexpr double kPercent = 100000.0;  // DrawingML percentages: 100000 = 100 %
constexpr double kMinRecompressSavings = 0.05;
constexpr std::uint64_t kMinSavingsBytes = 1024;
constexpr std::string_view kImageRelType = "/relationships/image";  // Transitional and Strict

// Extent in EMU.
struct Extent {
    double cx = 0;
    double cy = 0;
};

// What the uses of one media part ask of it.
struct Demand {
    Extent displayed;        // largest displayed extent per axis, EMU
    bool tiled = false;      // drawn at its own pixel size somewhere: keep the pixel grid
    bool inherited = false;  // a placeholder use without its own geometry
    bool foreign = false;    // referenced through a non-image relationship

    void include(const Extent& e) {
        displayed.cx = std::max(displayed.cx, e.cx);
        displayed.cy = std::max(displayed.cy, e.cy);
    }
};

std::string_view local_name(std::string_view name) {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool is(const pugi::xml_node& node, std::string_view local) {
    return local_name(node.name()) == local;
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view local) {
    for (const auto& c : node.children()) {
        if (is(c, local)) {
            return c;
        }
    }
    return {};
}

double attr(const pugi::xml_node& node, const char* name) {
    return node.attribute(name).as_double(0.0);
}

std::optional<Extent> ext_of(const pugi::xml_node& xfrm) {
    const auto ext = child(xfrm, "ext");
    if (!ext) {
        return std::nullopt;
    }
    const Extent e{attr(ext, "cx"), attr(ext, "cy")};
    if (e.cx <= 0 || e.cy <= 0) {
        return std::nullopt;
    }
    return e;
}

// The fraction of the image's width and height an a:srcRect / a:fillRect with
// these l/t/r/b insets leaves; at least 0.1 % so a degenerate rect can't blow
// the demand up without bound.
Extent kept_fraction(const pugi::xml_node& rect) {
    constexpr double kMin = 0.001;
    if (!rect) {
        return {1, 1};
    }
    return {std::max(kMin, 1 - (attr(rect, "l") + attr(rect, "r")) / kPercent),
            std::max(kMin, 1 - (attr(rect, "t") + attr(rect, "b")) / kPercent)};
}

// How much enclosing groups scale a shape's own extent: the product of each
// group's ext / chExt. A group with a degenerate child space (seen in the
// wild) doesn't scale.
Extent group_scale(pugi::xml_node shape) {
    Extent s{1, 1};
    for (auto g = shape.parent(); g; g = g.parent()) {
        if (!is(g, "grpSp")) {
            continue;
        }
        const auto xfrm = child(child(g, "grpSpPr"), "xfrm");
        const auto ext = ext_of(xfrm);
        const auto ch = child(xfrm, "chExt");
        const double ch_cx = attr(ch, "cx");
        const double ch_cy = attr(ch, "cy");
        if (ext && ch_cx > 0 && ch_cy > 0) {
            s.cx *= ext->cx / ch_cx;
            s.cy *= ext->cy / ch_cy;
        }
    }
    return s;
}

// The shape's p:ph (p:nvPicPr / p:nvSpPr / ... -> p:nvPr -> p:ph), if any.
pugi::xml_node placeholder_of(const pugi::xml_node& shape) {
    for (const auto& c : shape.children()) {
        if (local_name(c.name()).starts_with("nv")) {
            if (auto ph = child(child(c, "nvPr"), "ph")) {
                return ph;
            }
        }
    }
    return {};
}

// The slide being sized, and where its placeholders' inherited geometry
// comes from.
struct SlideContext {
    const std::string& part;
    Extent canvas;
    PlaceholderInheritance& inheritance;
};

// The extent a placeholder without its own a:xfrm inherits from its layout
// or master; nullopt when neither declares one.
std::optional<Extent> inherited_ext(const pugi::xml_node& ph, SlideContext& ctx) {
    std::optional<std::string> type;
    if (auto a = ph.attribute("type")) {
        type = a.as_string("");
    }
    std::optional<int> idx;
    if (auto a = ph.attribute("idx")) {
        idx = a.as_int(0);
    }
    const auto g = ctx.inheritance.lookup(ctx.part, type, idx).geometry;
    if (!g || g->ext_cx <= 0 || g->ext_cy <= 0) {
        return std::nullopt;
    }
    return Extent{static_cast<double>(g->ext_cx), static_cast<double>(g->ext_cy)};
}

// One a:blip r:embed use on a slide.
void add_blip_use(const pugi::xml_node& blip, SlideContext& ctx, Demand& demand) {
    const Extent& canvas = ctx.canvas;
    const auto fill = blip.parent();
    if (!is(fill, "blipFill")) {
        demand.include(canvas);
        return;
    }
    if (child(fill, "tile")) {
        demand.tiled = true;
        return;
    }
    const auto owner = fill.parent();
    pugi::xml_node shape;
    pugi::xml_node sp_pr;
    if (is(owner, "pic")) {
        shape = owner;
        sp_pr = child(owner, "spPr");
    } else if (is(owner, "spPr")) {
        shape = owner.parent();
        sp_pr = owner;
    } else {
        demand.include(canvas);  // background (bgPr), table cell (tcPr), ...
        return;
    }
    auto ext = ext_of(child(sp_pr, "xfrm"));
    if (!ext) {
        const auto ph = placeholder_of(shape);
        if (!ph) {
            demand.include(canvas);
            return;
        }
        ext = inherited_ext(ph, ctx);
        if (!ext) {
            demand.inherited = true;  // a placeholder with nothing to inherit from
            return;
        }
    }
    const Extent scale = group_scale(shape);
    const Extent visible = kept_fraction(child(fill, "srcRect"));
    const Extent filled = kept_fraction(child(child(fill, "stretch"), "fillRect"));
    demand.include(
        {ext->cx * scale.cx * filled.cx / visible.cx, ext->cy * scale.cy * filled.cy / visible.cy});
}

// Every use of relationship `rel_id` in a slide's XML.
void add_slide_uses(const pugi::xml_document& doc,
                    const std::string& rel_id,
                    SlideContext& ctx,
                    Demand& demand) {
    const Extent& canvas = ctx.canvas;
    bool found = false;
    for (const auto& node : doc.select_nodes("//*")) {
        const auto el = node.node();
        for (const auto& a : el.attributes()) {
            const std::string_view name = a.name();
            if (name.find(':') == std::string_view::npos || rel_id != a.value()) {
                continue;
            }
            found = true;
            if (is(el, "blip") && local_name(name) == "embed") {
                add_blip_use(el, ctx, demand);
            } else {
                demand.include(canvas);  // a14:imgLayer, video/audio, ... sized conservatively
            }
        }
    }
    if (!found) {
        demand.include(canvas);  // declared but not found where we look: assume the worst
    }
}

Extent read_size(const pugi::xml_node& node) {
    return {attr(node, "cx"), attr(node, "cy")};
}

// Slide canvas, and the larger of slide and notes pages for parts that may
// draw on either.
struct Canvases {
    Extent slide{10 * kEmuPerInch, 7.5 * kEmuPerInch};
    Extent any = slide;
};

Canvases read_canvases(const Package& pkg) {
    Canvases c;
    std::string pres = "ppt/presentation.xml";
    for (const auto& rel : pkg.relationships("")) {
        if (rel.type.ends_with("/officeDocument") && !rel.external) {
            pres = rel.target;
        }
    }
    const auto bytes = pkg.read_part(pres);
    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.data(), bytes.size())) {
        return c;
    }
    const auto root = doc.document_element();
    const Extent slide = read_size(child(root, "sldSz"));
    if (slide.cx > 0 && slide.cy > 0) {
        c.slide = slide;
    }
    const Extent notes = read_size(child(root, "notesSz"));
    c.any = {std::max(c.slide.cx, notes.cx), std::max(c.slide.cy, notes.cy)};
    return c;
}

bool is_slide(std::string_view part) {
    return part.starts_with("ppt/slides/") && part.ends_with(".xml");
}

bool is_layout_or_master(std::string_view part) {
    return part.starts_with("ppt/slideLayouts/") || part.starts_with("ppt/slideMasters/");
}

// Parsed source parts, loaded on first use.
class SourceCache {
 public:
    explicit SourceCache(const Package& pkg) : pkg_(pkg) {}

    const pugi::xml_document* get(const std::string& part) {
        auto it = docs_.find(part);
        if (it == docs_.end()) {
            auto doc = std::make_unique<pugi::xml_document>();
            const auto bytes = pkg_.read_part(part);
            if (!doc->load_buffer(bytes.data(), bytes.size())) {
                doc.reset();
            }
            it = docs_.emplace(part, std::move(doc)).first;
        }
        return it->second.get();
    }

 private:
    const Package& pkg_;
    std::map<std::string, std::unique_ptr<pugi::xml_document>> docs_;
};

Demand demand_for(const PartGraph& graph,
                  const std::string& media,
                  const Canvases& canvases,
                  SourceCache& sources,
                  PlaceholderInheritance& inheritance) {
    Demand demand;
    for (const auto& ref : graph.references_to(media)) {
        // Transitional or Strict OOXML image relationship; anything else
        // (hdphoto, OLE preview, ...) may expect these exact bytes.
        if (!ref.rel.type.ends_with(kImageRelType)) {
            demand.foreign = true;
            continue;
        }
        if (is_slide(ref.source)) {
            if (const auto* doc = sources.get(ref.source)) {
                SlideContext ctx{ref.source, canvases.slide, inheritance};
                add_slide_uses(*doc, ref.rel.id, ctx, demand);
            } else {
                demand.include(canvases.slide);
            }
        } else if (is_layout_or_master(ref.source)) {
            demand.include(canvases.slide);
        } else {
            demand.include(canvases.any);
        }
    }
    return demand;
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

// EXIF orientations 5-8 transpose the image: stored width is displayed height.
bool transposed(const Image& img) {
    return img.exif_orientation >= 5 && img.exif_orientation <= 8;
}

// The stored pixel size that gives `displayed` (EMU) at `dpi`, keeping the
// aspect ratio; nullopt when the image already has no more than that.
std::optional<ImageDimensions> target_size(const Image& img, Extent displayed, double dpi) {
    if (displayed.cx <= 0 || displayed.cy <= 0) {
        return std::nullopt;  // no sized use (e.g. only tiled ones): nothing to go by
    }
    if (transposed(img)) {
        std::swap(displayed.cx, displayed.cy);
    }
    const double need_w = std::ceil(displayed.cx / kEmuPerInch * dpi - 1e-6);
    const double need_h = std::ceil(displayed.cy / kEmuPerInch * dpi - 1e-6);
    const double w = img.width;
    const double h = img.height;
    const double scale = std::max(need_w / w, need_h / h);
    if (scale >= 1.0) {
        return std::nullopt;
    }
    const auto new_w = static_cast<std::uint32_t>(std::max(1.0, std::ceil(w * scale - 1e-6)));
    const auto new_h = static_cast<std::uint32_t>(std::max(1.0, std::ceil(h * scale - 1e-6)));
    if (new_w >= img.width || new_h >= img.height) {
        return std::nullopt;
    }
    return ImageDimensions{new_w, new_h};
}

void skip(PassResult& result, const std::string& part, std::uint64_t size, std::string why) {
    result.actions.push_back({part, "skipped", std::move(why), size, size});
}

// `img` encoded in `format`, if that's worth replacing a part of `before`
// bytes with: never larger, at least kMinSavingsBytes smaller, and for a JPEG
// re-encoded at its own size (`same_size`) at least kMinRecompressSavings too.
std::optional<std::vector<std::byte>> reencode(const Image& img,
                                               ImageFormat format,
                                               std::uint64_t before,
                                               bool same_size,
                                               const OptimizeOptions& opts) {
    auto encoded = format == ImageFormat::Jpeg
                       ? encode_jpeg(img, {.quality = opts.jpeg_quality, .progressive = true})
                       : encode_png(img);
    const auto after = static_cast<std::uint64_t>(encoded.size());
    const bool lossy_same_size = same_size && format == ImageFormat::Jpeg;
    const double ratio_limit =
        static_cast<double>(before) * (lossy_same_size ? 1 - kMinRecompressSavings : 1.0);
    if (encoded.empty() || static_cast<double>(after) >= ratio_limit ||
        before - after < kMinSavingsBytes) {
        return std::nullopt;  // never grow, and don't rewrite a part for crumbs
    }
    return encoded;
}

void process(const Package& pkg,
             const std::string& part,
             ImageFormat format,
             const Demand& demand,
             const OptimizeOptions& opts,
             PassResult& result) {
    const auto bytes = pkg.read_part(part);
    const std::uint64_t before = bytes.size();
    if (sniff_image_format(bytes) != format) {
        return;  // content type and bytes disagree: not ours to touch
    }
    if (demand.inherited) {
        skip(result, part, before, "inherited geometry, skipped");
        return;
    }
    auto decoded = format == ImageFormat::Jpeg ? decode_jpeg(bytes) : decode_png(bytes);
    if (!decoded.image) {
        skip(result, part, before, decoded.reason);
        return;
    }
    const Image& img = *decoded.image;

    const double dpi = opts.max_dpi > 0 ? opts.max_dpi : kCanvasDpi * opts.max_scale;
    const auto target =
        demand.tiled || dpi <= 0 ? std::nullopt : target_size(img, demand.displayed, dpi);
    if (target) {
        const Image small = downsample_area(img, target->width, target->height);
        if (auto encoded = reencode(small, format, before, false, opts)) {
            result.actions.push_back(
                {part,
                 "downsampled",
                 std::format("{}x{} -> {}x{}", img.width, img.height, small.width, small.height),
                 before,
                 encoded->size()});
            result.changes.replaced.emplace(part, std::move(*encoded));
            return;
        }
        // Downsampling didn't pay: blending can turn a palette screenshot
        // into a truecolour PNG larger than the original. A re-encode at
        // the original size may still.
    }
    if (auto encoded = reencode(img, format, before, true, opts)) {
        result.actions.push_back({part,
                                  "recompressed",
                                  std::format("{}x{}", img.width, img.height),
                                  before,
                                  encoded->size()});
        result.changes.replaced.emplace(part, std::move(*encoded));
    }
}

}  // namespace

PassResult optimize_images(const Package& pkg,
                           const PartGraph& graph,
                           const OptimizeOptions& opts) {
    PassResult result;
    const Canvases canvases = read_canvases(pkg);
    SourceCache sources(pkg);
    PlaceholderInheritance inheritance(pkg);
    for (const auto& part : graph.reachable()) {
        const auto format = media_format(pkg, part);
        if (!format) {
            continue;
        }
        const Demand demand = demand_for(graph, part, canvases, sources, inheritance);
        if (demand.foreign) {
            continue;
        }
        process(pkg, part, *format, demand, opts, result);
    }
    return result;
}

}  // namespace pptctl::detail
