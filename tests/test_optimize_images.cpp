// Tests for the "images" optimize pass (#103).
//
// Most cases start from bloat_deck.pptx (10 x 7.5 in canvas; one picture per
// slide: image1.png 1600x1200 shown 1 in wide, image2.png 200x150 shown 0.5 in
// wide, image3.png 64x48 shown 4 in wide) and write a variant of it with
// Package::write: a different image behind a slide, an edited slide or layout.
// At the default density (2 x 96 DPI) one displayed inch keeps 192 pixels.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/image.hpp>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/slide.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/optimize_internal.hpp"
#include "../src/lib/resample.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_optimize_images_" + run + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
    static inline int counter = 0;
};

std::string to_string(const Bytes& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
    return out;
}

Bytes to_bytes(std::string_view s) {
    Bytes out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// Pseudo-random pixels: photo-like for the codecs (no long runs), so sizes
// track pixel counts.
pptctl::Image noise(std::uint32_t w, std::uint32_t h, std::uint8_t channels, std::uint32_t seed) {
    pptctl::Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.pixels.resize(std::size_t{w} * h * channels);
    std::uint32_t state = seed;
    for (auto& p : img.pixels) {
        state = state * 1103515245U + 12345U;
        p = static_cast<std::uint8_t>((state >> 16) & 0xFF);
    }
    return img;
}

// Smooth RGBA gradient whose alpha falls from opaque to transparent.
pptctl::Image alpha_gradient(std::uint32_t w, std::uint32_t h) {
    pptctl::Image img;
    img.width = w;
    img.height = h;
    img.channels = 4;
    img.pixels.resize(std::size_t{w} * h * 4);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            img.pixels[i++] = static_cast<std::uint8_t>(x * 255 / w);
            img.pixels[i++] = static_cast<std::uint8_t>(y * 255 / h);
            img.pixels[i++] = static_cast<std::uint8_t>((x + y) * 255 / (w + h));
            img.pixels[i++] = static_cast<std::uint8_t>(255 - (x * 255 / w));
        }
    }
    return img;
}

// Edits accumulated against a base package, then written out and reopened.
class Variant {
 public:
    explicit Variant(const char* base = "bloat_deck.pptx") {
        REQUIRE(base_.open(fixture(base)));
    }

    [[nodiscard]] std::string read(const std::string& part) const {
        const auto it = changes_.replaced.find(part);
        return to_string(it != changes_.replaced.end() ? it->second : base_.read_part(part));
    }

    void set(const std::string& part, const Bytes& bytes) {
        changes_.replaced[part] = bytes;
    }

    // Replace the first occurrence of `from` in `part`; it must be there.
    void edit(const std::string& part, std::string_view from, std::string_view to) {
        auto xml = read(part);
        const auto at = xml.find(from);
        REQUIRE_MESSAGE(at != std::string::npos, part << " has no " << from);
        xml.replace(at, from.size(), to);
        set(part, to_bytes(xml));
    }

    // Add a media part with an explicit content type.
    void add_media(const std::string& name, const Bytes& bytes, const std::string& content_type) {
        set("ppt/media/" + name, bytes);
        const auto types = to_bytes(read("[Content_Types].xml"));
        set("[Content_Types].xml",
            pptctl::rewrite_content_types(types, {}, {{"ppt/media/" + name, content_type}}));
    }

    // Point slide `n`'s picture (rId2) at media `name`.
    void retarget_slide(int n, const std::string& from, const std::string& name) {
        edit("ppt/slides/_rels/slide" + std::to_string(n) + ".xml.rels",
             "../media/" + from,
             "../media/" + name);
    }

    // Write the variant and open it.
    pptctl::Package& open() {
        const auto path = (dir_.path / "variant.pptx").string();
        REQUIRE(base_.write(path, changes_));
        REQUIRE(out_.open(path));
        return out_;
    }

    [[nodiscard]] const fs::path& dir() const noexcept {
        return dir_.path;
    }

 private:
    ScratchDir dir_;
    pptctl::Package base_;
    pptctl::PartChanges changes_;
    pptctl::Package out_;
};

pptctl::PassResult run(const pptctl::Package& pkg, const pptctl::OptimizeOptions& opts = {}) {
    return pptctl::detail::optimize_images(pkg, pptctl::PartGraph::build(pkg), opts);
}

const pptctl::OptimizeAction* action_for(const pptctl::PassResult& r, std::string_view part) {
    const auto it = std::find_if(
        r.actions.begin(), r.actions.end(), [&](const auto& a) { return a.part == part; });
    return it == r.actions.end() ? nullptr : &*it;
}

pptctl::ImageDimensions dims_of(const Bytes& bytes) {
    const auto d = pptctl::probe_image_dimensions(bytes);
    REQUIRE(d.has_value());
    return *d;
}

// The replaced bytes of `part`, which must have been replaced.
const Bytes& replaced(const pptctl::PassResult& r, const std::string& part) {
    const auto it = r.changes.replaced.find(part);
    REQUIRE_MESSAGE(it != r.changes.replaced.end(), part << " was not replaced");
    return it->second;
}

constexpr std::string_view kSlide2Ext = R"(<a:ext cx="457200" cy="342900"/>)";

void push_be32(Bytes& b, std::uint32_t v) {
    for (const int shift : {24, 16, 8, 0}) {
        b.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
    }
}

void push_chunk(Bytes& b, std::string_view tag, const Bytes& data) {
    push_be32(b, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = b.size();
    for (const char ch : tag) {
        b.push_back(static_cast<std::byte>(ch));
    }
    b.insert(b.end(), data.begin(), data.end());
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = start; i < b.size(); ++i) {
        c ^= static_cast<std::uint32_t>(b[i]);
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
    }
    push_be32(b, c ^ 0xFFFFFFFFU);
}

// A palette PNG of `colours` (RGB triples) picked per pixel by a fixed LCG,
// stored without compression (zlib stored blocks): bulky, but every pixel is
// one of a few colours, so max deflate shrinks it a lot. Like a screenshot
// saved by a careless tool.
Bytes stored_palette_png(std::uint32_t w,
                         std::uint32_t h,
                         const std::vector<std::uint8_t>& colours) {
    Bytes raw;
    std::uint32_t state = 7;
    const auto n = static_cast<std::uint32_t>(colours.size() / 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(std::byte{0});  // filter: none
        for (std::uint32_t x = 0; x < w; ++x) {
            state = state * 1103515245U + 12345U;
            raw.push_back(static_cast<std::byte>((state >> 16) % n));
        }
    }
    Bytes z{std::byte{0x78}, std::byte{0x01}};
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(65535, raw.size() - at));
        const bool last = at + len == raw.size();
        z.push_back(static_cast<std::byte>(last ? 1 : 0));
        for (const std::uint32_t v : {len, ~len}) {
            z.push_back(static_cast<std::byte>(v & 0xFFU));
            z.push_back(static_cast<std::byte>((v >> 8) & 0xFFU));
        }
        z.insert(z.end(),
                 raw.begin() + static_cast<std::ptrdiff_t>(at),
                 raw.begin() + static_cast<std::ptrdiff_t>(at + len));
    }
    std::uint32_t a = 1;
    std::uint32_t sum = 0;
    for (const auto byte : raw) {
        a = (a + static_cast<std::uint32_t>(byte)) % 65521U;
        sum = (sum + a) % 65521U;
    }
    push_be32(z, (sum << 16) | a);

    Bytes png{std::byte{0x89},
              std::byte{'P'},
              std::byte{'N'},
              std::byte{'G'},
              std::byte{0x0D},
              std::byte{0x0A},
              std::byte{0x1A},
              std::byte{0x0A}};
    Bytes ihdr;
    push_be32(ihdr, w);
    push_be32(ihdr, h);
    for (const int v : {8, 3, 0, 0, 0}) {  // 8-bit palette
        ihdr.push_back(static_cast<std::byte>(v));
    }
    push_chunk(png, "IHDR", ihdr);
    Bytes plte;
    for (const auto c : colours) {
        plte.push_back(static_cast<std::byte>(c));
    }
    push_chunk(png, "PLTE", plte);
    push_chunk(png, "IDAT", z);
    push_chunk(png, "IEND", {});
    return png;
}

}  // namespace

// ── resampler ─────────────────────────────────────────────────────────────────

TEST_CASE("downsample_area: averages the source pixels each output pixel covers") {
    pptctl::Image src;
    src.width = 3;
    src.height = 2;
    src.channels = 1;
    src.pixels = {0, 90, 180, 30, 120, 210};
    src.exif_orientation = 6;
    const auto out = pptctl::detail::downsample_area(src, 2, 1);
    CHECK(out.width == 2);
    CHECK(out.height == 1);
    // Left output covers columns 0 and half of 1: (0 + 30 + (90 + 120) / 2) / 3 = 45.
    // Right: ((90 + 120) / 2 + 180 + 210) / 3 = 165.
    CHECK(out.pixels == std::vector<std::uint8_t>{45, 165});
    CHECK(out.exif_orientation == 6);  // metadata carried over
}

TEST_CASE("downsample_area: transparent pixels don't bleed their colour") {
    pptctl::Image src;
    src.width = 2;
    src.height = 1;
    src.channels = 4;
    src.pixels = {255, 0, 0, 255, 0, 255, 0, 0};  // opaque red, transparent green
    const auto out = pptctl::detail::downsample_area(src, 1, 1);
    CHECK(out.pixels == std::vector<std::uint8_t>{255, 0, 0, 128});
}

// ── sizing ────────────────────────────────────────────────────────────────────

TEST_CASE("optimize images: bloat_deck's oversampled PNGs shrink to 2x their displayed size") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("bloat_deck.pptx")));
    const auto r = run(pkg);

    // image1: 1600x1200 shown 1 x 0.75 in -> 192x144.
    const auto& one = replaced(r, "ppt/media/image1.png");
    CHECK(dims_of(one).width == 192);
    CHECK(dims_of(one).height == 144);
    CHECK(one.size() < *pkg.part_size("ppt/media/image1.png"));
    const auto* a1 = action_for(r, "ppt/media/image1.png");
    REQUIRE(a1 != nullptr);
    CHECK(a1->kind == "downsampled");
    CHECK(a1->detail == "1600x1200 -> 192x144");
    CHECK(a1->bytes_before == *pkg.part_size("ppt/media/image1.png"));
    CHECK(a1->bytes_after == one.size());

    // image2: 200x150 noise shown 0.5 x 0.375 in -> 96x72.
    const auto& two = replaced(r, "ppt/media/image2.png");
    CHECK(dims_of(two).width == 96);
    CHECK(dims_of(two).height == 72);
    CHECK(two.size() < *pkg.part_size("ppt/media/image2.png"));

    // image3: 64x48 shown 4 in wide is under-sampled already, so it is never
    // upscaled; re-encoding its 124 bytes saves less than the 1 KiB floor
    // that makes a rewrite worth it: untouched and unreported.
    CHECK_FALSE(r.changes.replaced.contains("ppt/media/image3.png"));
    CHECK(action_for(r, "ppt/media/image3.png") == nullptr);
    CHECK(r.changes.removed.empty());
    CHECK(r.changes.replaced.size() == r.actions.size());
}

TEST_CASE("optimize images: --max-scale and --max-dpi set the kept density") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("bloat_deck.pptx")));

    pptctl::OptimizeOptions one_x;
    one_x.max_scale = 1.0;  // 96 DPI
    const auto a = run(pkg, one_x);
    CHECK(dims_of(replaced(a, "ppt/media/image1.png")).width == 96);
    CHECK(dims_of(replaced(a, "ppt/media/image1.png")).height == 72);

    pptctl::OptimizeOptions dpi;
    dpi.max_dpi = 300.0;  // overrides max_scale
    dpi.max_scale = 1.0;
    const auto b = run(pkg, dpi);
    CHECK(dims_of(replaced(b, "ppt/media/image1.png")).width == 300);
    CHECK(dims_of(replaced(b, "ppt/media/image1.png")).height == 225);
}

TEST_CASE("optimize images: a PNG that needs all its pixels is recompressed losslessly") {
    // At 5000 DPI image1 (1600x1200, shown 1 in wide) is under-sampled, so it
    // keeps its size; its loosely deflated 7 KiB still shrink at max deflate.
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("bloat_deck.pptx")));
    pptctl::OptimizeOptions opts;
    opts.max_dpi = 5000.0;
    const auto r = run(pkg, opts);
    const auto& one = replaced(r, "ppt/media/image1.png");
    CHECK(one.size() + 1024 <= *pkg.part_size("ppt/media/image1.png"));
    const auto* a = action_for(r, "ppt/media/image1.png");
    REQUIRE(a != nullptr);
    CHECK(a->kind == "recompressed");
    CHECK(a->detail == "1600x1200");
    const auto before = pptctl::decode_png(pkg.read_part("ppt/media/image1.png"));
    const auto after = pptctl::decode_png(one);
    REQUIRE(before.image.has_value());
    REQUIRE(after.image.has_value());
    CHECK(after.image->width == 1600);
    CHECK(after.image->pixels == before.image->pixels);
}

TEST_CASE("optimize images: a PNG that downsampling would grow is still recompressed") {
    // A 400x300 palette PNG of 16 noisy colours, stored uncompressed (118 KiB),
    // shown at 75 % of its pixels: downsampling blends the colours into a
    // truecolour PNG larger than the original, but a lossless recompress at
    // the original size still halves it. The pass must fall back to that
    // rather than leave the image as it was.
    std::vector<std::uint8_t> colours;
    for (int i = 0; i < 16; ++i) {
        colours.insert(colours.end(),
                       {static_cast<std::uint8_t>(i * 16),
                        static_cast<std::uint8_t>(255 - i * 16),
                        static_cast<std::uint8_t>((i * 97) % 256)});
    }
    const auto png = stored_palette_png(400, 300, colours);
    REQUIRE(pptctl::decode_png(png).image.has_value());
    Variant v;
    v.add_media("palette.png", png, "image/png");
    v.retarget_slide(2, "image2.png", "palette.png");
    // 1.5625 x 1.171875 in at 192 DPI: 300x225 px.
    v.edit("ppt/slides/slide2.xml", kSlide2Ext, R"(<a:ext cx="1428750" cy="1071563"/>)");
    const auto r = run(v.open());
    const auto* a = action_for(r, "ppt/media/palette.png");
    REQUIRE(a != nullptr);
    CHECK(a->kind == "recompressed");
    CHECK(a->detail == "400x300");
    const auto& out = replaced(r, "ppt/media/palette.png");
    CHECK(out.size() + 1024 <= png.size());
    const auto before = pptctl::decode_png(png);
    const auto after = pptctl::decode_png(out);
    REQUIRE(after.image.has_value());
    CHECK(after.image->width == 400);
    CHECK(after.image->pixels == before.image->pixels);
}

TEST_CASE("optimize images: the largest use of a shared image decides its size") {
    // image1 behind slide 1 (1 in wide) and slide 3 (4 x 3 in) -> 768x576.
    Variant v;
    v.retarget_slide(3, "image3.png", "image1.png");
    const auto r = run(v.open());
    const auto& one = replaced(r, "ppt/media/image1.png");
    CHECK(dims_of(one).width == 768);
    CHECK(dims_of(one).height == 576);
}

TEST_CASE("optimize images: a cropped picture keeps the visible region's resolution") {
    // 800x800 shown in a 1 x 1 in box with the left and right quarters cropped
    // away: the visible half must keep 192 px across, so the whole image keeps
    // 384. Ignoring the crop would give 192x192.
    Variant v;
    v.add_media("crop.jpeg", pptctl::encode_jpeg(noise(800, 800, 3, 1)), "image/jpeg");
    v.retarget_slide(2, "image2.png", "crop.jpeg");
    v.edit("ppt/slides/slide2.xml", kSlide2Ext, R"(<a:ext cx="914400" cy="914400"/>)");
    v.edit("ppt/slides/slide2.xml",
           R"(<a:blip r:embed="rId2"/>)",
           R"(<a:blip r:embed="rId2"/><a:srcRect l="25000" r="25000"/>)");
    const auto r = run(v.open());
    const auto& out = replaced(r, "ppt/media/crop.jpeg");
    CHECK(dims_of(out).width == 384);
    CHECK(dims_of(out).height == 384);
}

TEST_CASE("optimize images: a picture in a scaled group is sized as displayed") {
    // image2 (200x150) is 0.5 x 0.375 in in a group whose child space is
    // shown at twice its size: displayed 1 x 0.75 in -> 192x144.
    Variant v;
    v.edit("ppt/slides/slide2.xml",
           "<p:pic>",
           R"(<p:grpSp><p:nvGrpSpPr><p:cNvPr id="5" name="Group"/><p:cNvGrpSpPr/><p:nvPr/>)"
           R"(</p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/>)"
           R"(<a:ext cx="1828800" cy="1371600"/><a:chOff x="0" y="0"/>)"
           R"(<a:chExt cx="914400" cy="685800"/></a:xfrm></p:grpSpPr><p:pic>)");
    v.edit("ppt/slides/slide2.xml", "</p:pic>", "</p:pic></p:grpSp>");
    const auto r = run(v.open());
    const auto& out = replaced(r, "ppt/media/image2.png");
    CHECK(dims_of(out).width == 192);
    CHECK(dims_of(out).height == 144);
}

TEST_CASE("optimize images: an image in a layout background counts as full canvas") {
    // 10 x 7.5 in canvas at 192 DPI: 1920x1440.
    Variant v;
    v.add_media("bg.jpeg", pptctl::encode_jpeg(noise(2400, 1800, 3, 2)), "image/jpeg");
    const std::string layout = "ppt/slideLayouts/slideLayout7.xml";
    v.edit(layout,
           R"(<p:cSld name="Blank">)",
           R"(<p:cSld name="Blank"><p:bg><p:bgPr><a:blipFill><a:blip r:embed="rId9"/>)"
           R"(<a:stretch><a:fillRect/></a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg>)");
    v.edit("ppt/slideLayouts/_rels/slideLayout7.xml.rels",
           "</Relationships>",
           R"(<Relationship Id="rId9" )"
           R"(Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" )"
           R"(Target="../media/bg.jpeg"/></Relationships>)");
    const auto r = run(v.open());
    const auto& out = replaced(r, "ppt/media/bg.jpeg");
    CHECK(dims_of(out).width == 1920);
    CHECK(dims_of(out).height == 1440);
}

TEST_CASE("optimize images: a shape's picture fill is sized to the shape; a tiled one is not") {
    const std::string pic_open = R"(<p:pic><p:nvPicPr><p:cNvPr id="2" name="Picture 1")";
    const auto shape_with = [](std::string_view fill_mode) {
        return std::string(R"(<p:sp><p:nvSpPr><p:cNvPr id="3" name="Filled"/><p:cNvSpPr/>)") +
               R"(<p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="0"/>)" +
               R"(<a:ext cx="914400" cy="914400"/></a:xfrm><a:prstGeom prst="rect"/>)" +
               R"(<a:blipFill><a:blip r:embed="rId9"/>)" + std::string(fill_mode) +
               R"(</a:blipFill></p:spPr></p:sp>)";
    };
    const auto relationship =
        std::string(R"(<Relationship Id="rId9" )") +
        R"(Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" )" +
        R"(Target="../media/fill.jpeg"/></Relationships>)";

    SUBCASE("stretched: 1 x 1 in -> 192x192") {
        Variant v;
        v.add_media(
            "fill.jpeg", pptctl::encode_jpeg(noise(600, 600, 3, 3), {.quality = 95}), "image/jpeg");
        v.edit("ppt/slides/slide2.xml",
               pic_open,
               shape_with("<a:stretch><a:fillRect/></a:stretch>") + pic_open);
        v.edit("ppt/slides/_rels/slide2.xml.rels", "</Relationships>", relationship);
        const auto r = run(v.open());
        CHECK(dims_of(replaced(r, "ppt/media/fill.jpeg")).width == 192);
    }
    SUBCASE("tiled: the tile size is the image's own, so its pixels stay") {
        Variant v;
        v.add_media(
            "fill.jpeg", pptctl::encode_jpeg(noise(600, 600, 3, 3), {.quality = 95}), "image/jpeg");
        v.edit(
            "ppt/slides/slide2.xml",
            pic_open,
            shape_with(R"(<a:tile tx="0" ty="0" sx="100000" sy="100000" algn="tl"/>)") + pic_open);
        v.edit("ppt/slides/_rels/slide2.xml.rels", "</Relationships>", relationship);
        const auto r = run(v.open());
        // Recompressed from quality 95 to 85 at the same size, never downsampled.
        const auto& out = replaced(r, "ppt/media/fill.jpeg");
        CHECK(dims_of(out).width == 600);
        CHECK(dims_of(out).height == 600);
        REQUIRE(action_for(r, "ppt/media/fill.jpeg") != nullptr);
        CHECK(action_for(r, "ppt/media/fill.jpeg")->kind == "recompressed");
    }
}

// ── formats ───────────────────────────────────────────────────────────────────

TEST_CASE("optimize images: JPEG keeps its EXIF orientation and resizes in stored orientation") {
    // Stored 400x300 with orientation 6 (rotate 90 CW): it displays as 300x400
    // portrait, here in a 0.75 x 1 in box -> 144x192 displayed, so the stored
    // grid becomes 192x144 and the tag stays.
    auto photo = noise(400, 300, 3, 4);
    photo.exif_orientation = 6;
    Variant v;
    v.add_media("photo.jpeg", pptctl::encode_jpeg(photo), "image/jpeg");
    v.retarget_slide(2, "image2.png", "photo.jpeg");
    v.edit("ppt/slides/slide2.xml", kSlide2Ext, R"(<a:ext cx="685800" cy="914400"/>)");
    const auto r = run(v.open());
    const auto decoded = pptctl::decode_jpeg(replaced(r, "ppt/media/photo.jpeg"));
    REQUIRE_MESSAGE(decoded.image.has_value(), decoded.reason);
    CHECK(decoded.image->exif_orientation == 6);
    CHECK(decoded.image->width == 192);
    CHECK(decoded.image->height == 144);
    CHECK(action_for(r, "ppt/media/photo.jpeg")->detail == "400x300 -> 192x144");
}

TEST_CASE("optimize images: a PNG with alpha stays a PNG with alpha") {
    Variant v;
    v.set("ppt/media/image2.png", pptctl::encode_png(alpha_gradient(400, 400)));
    v.edit("ppt/slides/slide2.xml", kSlide2Ext, R"(<a:ext cx="914400" cy="914400"/>)");
    const auto r = run(v.open());
    const auto& out = replaced(r, "ppt/media/image2.png");
    CHECK(pptctl::sniff_image_format(out) == pptctl::ImageFormat::Png);
    const auto decoded = pptctl::decode_png(out);
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->width == 192);
    CHECK(decoded.image->channels == 4);
    const auto& px = decoded.image->pixels;
    std::uint8_t min_alpha = 255;
    std::uint8_t max_alpha = 0;
    for (std::size_t i = 3; i < px.size(); i += 4) {
        min_alpha = std::min(min_alpha, px[i]);
        max_alpha = std::max(max_alpha, px[i]);
    }
    CHECK(min_alpha < 10);
    CHECK(max_alpha > 245);
}

TEST_CASE("optimize images: never grows an image") {
    // A quality-40 JPEG shown large needs no downsampling, and re-encoding it
    // at quality 85 would only add bytes: the original stays.
    Variant v;
    v.add_media(
        "small.jpeg", pptctl::encode_jpeg(noise(300, 225, 3, 5), {.quality = 40}), "image/jpeg");
    v.retarget_slide(3, "image3.png", "small.jpeg");
    const auto r = run(v.open());
    CHECK_FALSE(r.changes.replaced.contains("ppt/media/small.jpeg"));
    CHECK(action_for(r, "ppt/media/small.jpeg") == nullptr);
    for (const auto& a : r.actions) {
        CHECK(a.bytes_after <= a.bytes_before);
    }
}

TEST_CASE("optimize images: unsupported formats are left untouched") {
    Variant v;
    // GIF: not a candidate at all.
    v.add_media("anim.gif",
                to_bytes(std::string_view("GIF89a\x10\x00\x10\x00\x00\x00\x00;", 14)),
                "image/gif");
    v.retarget_slide(1, "image1.png", "anim.gif");
    // A JPEG the codec refuses (truncated here; CMYK and 12-bit JPEGs go the
    // same way): left as is and reported as skipped, with the reason.
    constexpr std::array<std::uint8_t, 4> kSoi = {0xFF, 0xD8, 0xFF, 0xEE};
    Bytes bad(kSoi.size());
    std::transform(kSoi.begin(), kSoi.end(), bad.begin(), [](auto b) { return std::byte{b}; });
    v.add_media("bad.jpeg", bad, "image/jpeg");
    v.retarget_slide(2, "image2.png", "bad.jpeg");
    const auto r = run(v.open());
    CHECK_FALSE(r.changes.replaced.contains("ppt/media/anim.gif"));
    CHECK(action_for(r, "ppt/media/anim.gif") == nullptr);
    CHECK_FALSE(r.changes.replaced.contains("ppt/media/bad.jpeg"));
    const auto* skipped = action_for(r, "ppt/media/bad.jpeg");
    REQUIRE(skipped != nullptr);
    CHECK(skipped->kind == "skipped");
    CHECK(skipped->bytes_after == skipped->bytes_before);
}

TEST_CASE("optimize images: a placeholder picture is sized by the geometry it inherits") {
    // image2 (200x150) in a picture placeholder (idx 1) without its own
    // a:xfrm; the slide's layout places that placeholder at 0.5 x 0.375 in,
    // as slide 2's explicit geometry did: 96x72 either way.
    Variant v;
    v.edit("ppt/slides/slide2.xml",
           R"(<p:nvPr/></p:nvPicPr>)",
           R"(<p:nvPr><p:ph type="pic" idx="1"/></p:nvPr></p:nvPicPr>)");
    v.edit("ppt/slides/slide2.xml",
           R"(<a:xfrm><a:off x="914400" y="914400"/><a:ext cx="457200" cy="342900"/></a:xfrm>)",
           "");
    v.edit("ppt/slideLayouts/slideLayout7.xml",
           "</p:spTree>",
           R"(<p:sp><p:nvSpPr><p:cNvPr id="9" name="Picture Placeholder 8"/><p:cNvSpPr/>)"
           R"(<p:nvPr><p:ph type="pic" idx="1"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm>)"
           R"(<a:off x="914400" y="914400"/><a:ext cx="457200" cy="342900"/></a:xfrm></p:spPr>)"
           R"(</p:sp></p:spTree>)");
    const auto r = run(v.open());
    const auto& out = replaced(r, "ppt/media/image2.png");
    CHECK(dims_of(out).width == 96);
    CHECK(dims_of(out).height == 72);
}

TEST_CASE("optimize images: a placeholder with nothing to inherit from is skipped") {
    // A header placeholder: no slide layout or master carries one.
    Variant v;
    v.edit("ppt/slides/slide2.xml",
           R"(<p:nvPr/></p:nvPicPr>)",
           R"(<p:nvPr><p:ph type="hdr"/></p:nvPr></p:nvPicPr>)");
    v.edit("ppt/slides/slide2.xml",
           R"(<a:xfrm><a:off x="914400" y="914400"/><a:ext cx="457200" cy="342900"/></a:xfrm>)",
           "");
    const auto r = run(v.open());
    CHECK_FALSE(r.changes.replaced.contains("ppt/media/image2.png"));
    const auto* skipped = action_for(r, "ppt/media/image2.png");
    REQUIRE(skipped != nullptr);
    CHECK(skipped->kind == "skipped");
    CHECK(skipped->detail.find("inherited geometry") != std::string::npos);
    // The other slides' images are still processed.
    CHECK(r.changes.replaced.contains("ppt/media/image1.png"));
}

TEST_CASE("optimize images: images_deck — nothing grows, and what changes shrinks") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto r = run(pkg);
    for (const auto& [part, bytes] : r.changes.replaced) {
        CHECK(bytes.size() < *pkg.part_size(part));
    }
    CHECK(r.changes.removed.empty());
}

TEST_CASE("optimize images: a deck without pictures has nothing to do") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    const auto r = run(pkg);
    CHECK(r.actions.empty());
    CHECK(r.changes.replaced.empty());
}

// ── end to end ────────────────────────────────────────────────────────────────

TEST_CASE("optimize images: end to end, slides keep their text and shapes") {
    ScratchDir dir;
    const auto in = fixture("bloat_deck.pptx");
    const auto out = (dir.path / "out.pptx").string();
    pptctl::OptimizeOptions opts;
    opts.layouts = false;
    opts.fonts = false;
    opts.crop = false;
    opts.orphans = false;
    std::string error;
    const auto report = pptctl::optimize(in, out, opts, error);
    REQUIRE_MESSAGE(report.has_value(), error);
    REQUIRE(report->passes.size() == 1);
    CHECK(report->passes[0].id == "images");
    CHECK(report->passes[0].actions.size() == 2);
    CHECK(report->output_bytes < report->input_bytes);

    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(in));
    REQUIRE(b.open(out));
    CHECK(a.part_names().size() == b.part_names().size());
    for (int n = 1; n <= 3; ++n) {
        const auto part = "ppt/slides/slide" + std::to_string(n) + ".xml";
        CHECK(a.read_part(part) == b.read_part(part));
        pptctl::Slide sa;
        pptctl::Slide sb;
        REQUIRE(sa.load(a, part));
        REQUIRE(sb.load(b, part));
        const auto changes = pptctl::compare_slides(sa, sb, {});
        CHECK(changes.text.empty());
        CHECK(changes.shapes.empty());
    }
}
