// Tests for the "crop" optimize pass (#104).
//
// Most cases start from crop_deck.pptx (10 x 7.5 in canvas): slide 1 shows
// image1.png (160x120) cropped to l=25 % r=25 % b=50 %, i.e. the 80x60 region
// at (40, 0); slide 2 shows image2.png (120x90) twice, once cropped l=50 % and
// once uncropped. Variants are written with Package::write: a different image
// behind slide 1, an edited crop or fill.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;

constexpr std::string_view kSlide1 = "ppt/slides/slide1.xml";
constexpr std::string_view kSlide1Crop = R"(<a:srcRect l="25000" r="25000" b="50000"/>)";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_optimize_crop_" + run + "_" + std::to_string(counter++));
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

// Pseudo-random pixels, so the codecs can't compress a crop away.
pptctl::Image noise(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    pptctl::Image img;
    img.width = w;
    img.height = h;
    img.channels = 3;
    img.pixels.resize(std::size_t{w} * h * 3);
    std::uint32_t state = seed;
    for (auto& p : img.pixels) {
        state = state * 1103515245U + 12345U;
        p = static_cast<std::uint8_t>((state >> 16) & 0xFF);
    }
    return img;
}

// Edits accumulated against crop_deck.pptx, then written out and reopened.
class Variant {
 public:
    Variant() {
        REQUIRE(base_.open(fixture("crop_deck.pptx")));
    }

    [[nodiscard]] std::string read(const std::string& part) const {
        const auto it = changes_.replaced.find(part);
        return to_string(it != changes_.replaced.end() ? it->second : base_.read_part(part));
    }

    // Replace the first occurrence of `from` in `part`; it must be there.
    void edit(const std::string& part, std::string_view from, std::string_view to) {
        auto xml = read(part);
        const auto at = xml.find(from);
        REQUIRE_MESSAGE(at != std::string::npos, part << " has no " << from);
        xml.replace(at, from.size(), to);
        changes_.replaced[part] = to_bytes(xml);
    }

    // Show a new JPEG on slide 1 instead of image1.png.
    void jpeg_on_slide1(const Bytes& jpeg) {
        changes_.replaced["ppt/media/photo.jpeg"] = jpeg;
        const auto types = to_bytes(read("[Content_Types].xml"));
        changes_.replaced["[Content_Types].xml"] =
            pptctl::rewrite_content_types(types, {}, {{"ppt/media/photo.jpeg", "image/jpeg"}});
        edit("ppt/slides/_rels/slide1.xml.rels", "../media/image1.png", "../media/photo.jpeg");
    }

    pptctl::Package& open() {
        const auto path = (dir_.path / "variant.pptx").string();
        REQUIRE(base_.write(path, changes_));
        REQUIRE(out_.open(path));
        return out_;
    }

 private:
    ScratchDir dir_;
    pptctl::Package base_;
    pptctl::PartChanges changes_;
    pptctl::Package out_;
};

pptctl::PassResult run(const pptctl::Package& pkg) {
    return pptctl::detail::optimize_crop(pkg, pptctl::PartGraph::build(pkg), {});
}

const pptctl::OptimizeAction* action_for(const pptctl::PassResult& r, std::string_view part) {
    const auto it = std::find_if(
        r.actions.begin(), r.actions.end(), [&](const auto& a) { return a.part == part; });
    return it == r.actions.end() ? nullptr : &*it;
}

// The replaced bytes of `part`, which must have been replaced.
const Bytes& replaced(const pptctl::PassResult& r, std::string_view part) {
    const auto it = r.changes.replaced.find(std::string(part));
    REQUIRE_MESSAGE(it != r.changes.replaced.end(), part << " was not replaced");
    return it->second;
}

pptctl::ImageDimensions dims_of(const Bytes& bytes) {
    const auto d = pptctl::probe_image_dimensions(bytes);
    REQUIRE(d.has_value());
    return *d;
}

// The skip reason reported for `part`, which must have been skipped.
std::string skip_reason(const pptctl::PassResult& r, std::string_view part) {
    const auto* a = action_for(r, part);
    REQUIRE_MESSAGE(a != nullptr, part << " has no action");
    CHECK(a->kind == "skipped");
    CHECK_FALSE(r.changes.replaced.contains(std::string(part)));
    return a->detail;
}

std::string erase(std::string s, std::string_view what) {
    const auto at = s.find(what);
    REQUIRE(at != std::string::npos);
    return s.erase(at, what.size());
}

}  // namespace

TEST_CASE("optimize crop: a cropped PNG keeps only its visible pixels, and loses its srcRect") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("crop_deck.pptx")));
    const auto r = run(pkg);

    const auto* a = action_for(r, "ppt/media/image1.png");
    REQUIRE(a != nullptr);
    CHECK(a->kind == "cropped");
    CHECK(a->detail == "160x120 -> 80x60");
    CHECK(a->bytes_after < a->bytes_before);

    const auto out = pptctl::decode_png(replaced(r, "ppt/media/image1.png"));
    const auto src = pptctl::decode_png(pkg.read_part("ppt/media/image1.png"));
    REQUIRE(out.image.has_value());
    REQUIRE(src.image.has_value());
    REQUIRE(out.image->width == 80);
    REQUIRE(out.image->height == 60);
    bool same = true;
    for (std::uint32_t y = 0; y < 60; ++y) {
        const auto* from = &src.image->pixels[(std::size_t{y} * 160 + 40) * 3];
        const auto* to = &out.image->pixels[std::size_t{y} * 80 * 3];
        same = same && std::equal(from, from + 80 * 3, to);
    }
    CHECK(same);

    // The slide changes by exactly the srcRect: nothing else is re-serialized
    // away. (pugixml rewrites the declaration in its own quoting, as every
    // XML-editing pass does, so compare from the root element.)
    const auto from_root = [](const std::string& xml) { return xml.substr(xml.find("<p:sld")); };
    CHECK(from_root(to_string(replaced(r, kSlide1))) ==
          from_root(erase(to_string(pkg.read_part(kSlide1)), kSlide1Crop)));
}

TEST_CASE("optimize crop: an image shown with different crops is skipped") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("crop_deck.pptx")));
    const auto r = run(pkg);
    CHECK(skip_reason(r, "ppt/media/image2.png") == "2 uses with different crops");
    CHECK_FALSE(r.changes.replaced.contains("ppt/slides/slide2.xml"));
}

TEST_CASE("optimize crop: the same crop on every use crops once and rewrites each use") {
    // Slide 2 shows image1 twice, with slide 1's crop on both pictures.
    Variant v;
    v.edit("ppt/slides/_rels/slide2.xml.rels", "../media/image2.png", "../media/image1.png");
    v.edit("ppt/slides/slide2.xml", R"(<a:srcRect l="50000"/>)", kSlide1Crop);
    v.edit("ppt/slides/slide2.xml",
           R"(<a:blip r:embed="rId2"/><a:stretch>)",
           std::string(R"(<a:blip r:embed="rId2"/>)") + std::string(kSlide1Crop) + "<a:stretch>");
    const auto r = run(v.open());
    CHECK(dims_of(replaced(r, "ppt/media/image1.png")).width == 80);
    CHECK(r.actions.size() == 1);
    for (const auto* part : {"ppt/slides/slide1.xml", "ppt/slides/slide2.xml"}) {
        CHECK(to_string(replaced(r, part)).find("srcRect") == std::string::npos);
    }
}

TEST_CASE("optimize crop: a JPEG is cropped losslessly, keeping the block-grid remainder") {
    // 400x300 4:2:0 JPEG under slide 1's crop: visible x 100..300, y 0..150.
    // The left edge snaps out to the 16 px grid at 96, so 204x150 is kept and
    // the srcRect keeps the 4 px remainder: 4 / 204 = 1.961 %.
    Variant v;
    v.jpeg_on_slide1(pptctl::encode_jpeg(noise(400, 300, 3)));
    const auto r = run(v.open());
    const auto* a = action_for(r, "ppt/media/photo.jpeg");
    REQUIRE(a != nullptr);
    CHECK(a->kind == "cropped");
    CHECK(a->detail == "400x300 -> 204x150");
    const auto dims = dims_of(replaced(r, "ppt/media/photo.jpeg"));
    CHECK(dims.width == 204);
    CHECK(dims.height == 150);
    CHECK(to_string(replaced(r, kSlide1)).find(R"(<a:srcRect l="1961"/>)") != std::string::npos);
}

TEST_CASE("optimize crop: a JPEG with an EXIF rotation is skipped") {
    auto img = noise(400, 300, 4);
    img.exif_orientation = 6;
    Variant v;
    v.jpeg_on_slide1(pptctl::encode_jpeg(img));
    const auto r = run(v.open());
    CHECK(skip_reason(r, "ppt/media/photo.jpeg") == "EXIF orientation 6");
    CHECK_FALSE(r.changes.replaced.contains(std::string(kSlide1)));
}

TEST_CASE("optimize crop: a tiled fill is skipped") {
    Variant v;
    v.edit(std::string(kSlide1), "<a:stretch><a:fillRect/></a:stretch>", "<a:tile/>");
    const auto r = run(v.open());
    CHECK(skip_reason(r, "ppt/media/image1.png") == "tiled fill");
}

TEST_CASE("optimize crop: a picture with an SVG original is skipped") {
    // The same srcRect crops the SVG; cropping only the PNG fallback would
    // leave the two showing different regions.
    Variant v;
    v.edit(
        std::string(kSlide1),
        R"(<a:blip r:embed="rId2"/>)",
        R"(<a:blip r:embed="rId2"><a:extLst><a:ext uri="{96DAC541-7B7A-43D3-8B79-37D633B846F1}">)"
        R"(<asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" )"
        R"(r:embed="rId9"/></a:ext></a:extLst></a:blip>)");
    const auto r = run(v.open());
    CHECK(skip_reason(r, "ppt/media/image1.png") == "cropped together with an SVG or effect layer");
}

TEST_CASE("optimize crop: a crop between pixel boundaries keeps its sub-pixel remainder") {
    // l=12.345 % of 160 px is 19.752 px: pixels from 19 on are kept (141x120),
    // and srcRect keeps 0.752 / 141 = 0.533 % so the visible region is exact.
    Variant v;
    v.edit(std::string(kSlide1), kSlide1Crop, R"(<a:srcRect l="12345"/>)");
    const auto r = run(v.open());
    const auto dims = dims_of(replaced(r, "ppt/media/image1.png"));
    CHECK(dims.width == 141);
    CHECK(dims.height == 120);
    CHECK(to_string(replaced(r, kSlide1)).find(R"(<a:srcRect l="533"/>)") != std::string::npos);
}

TEST_CASE("optimize crop: a negative inset is kept, rescaled to the smaller image") {
    // l=-10 % pads 16 px left of the image; r=50 % cuts its right half. The
    // 80x120 kept image needs l=-16/80=-20 % to show the same padding.
    Variant v;
    v.edit(std::string(kSlide1), kSlide1Crop, R"(<a:srcRect l="-10000" r="50000"/>)");
    const auto r = run(v.open());
    const auto dims = dims_of(replaced(r, "ppt/media/image1.png"));
    CHECK(dims.width == 80);
    CHECK(dims.height == 120);
    CHECK(to_string(replaced(r, kSlide1)).find(R"(<a:srcRect l="-20000"/>)") != std::string::npos);
}

TEST_CASE("optimize crop: decks without crops have nothing to do") {
    for (const auto* name : {"minimal.pptx", "bloat_deck.pptx", "images_deck.pptx"}) {
        pptctl::Package pkg;
        REQUIRE(pkg.open(fixture(name)));
        const auto r = run(pkg);
        CHECK(r.actions.empty());
        CHECK(r.changes.replaced.empty());
    }
}

TEST_CASE("optimize crop: end to end with images, slides keep their geometry") {
    ScratchDir dir;
    const auto in = fixture("crop_deck.pptx");
    const auto out = (dir.path / "out.pptx").string();
    pptctl::OptimizeOptions opts;
    opts.layouts = false;
    opts.fonts = false;
    opts.orphans = false;
    std::string error;
    const auto report = pptctl::optimize(in, out, opts, error);
    REQUIRE_MESSAGE(report.has_value(), error);
    REQUIRE(report->passes.size() == 2);
    CHECK(report->passes[0].id == "crop");
    CHECK(report->output_bytes < report->input_bytes);

    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(in));
    REQUIRE(b.open(out));
    CHECK(dims_of(b.read_part("ppt/media/image1.png")).width == 80);
    for (const auto* part : {"ppt/slides/slide1.xml", "ppt/slides/slide2.xml"}) {
        pptctl::Slide sa;
        pptctl::Slide sb;
        REQUIRE(sa.load(a, part));
        REQUIRE(sb.load(b, part));
        const auto changes = pptctl::compare_slides(sa, sb, {});
        CHECK(changes.text.empty());
        CHECK(changes.shapes.empty());
    }
}

TEST_CASE("optimize crop: --no-crop leaves cropped images whole") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    pptctl::OptimizeOptions opts;
    opts.crop = false;
    std::string error;
    const auto report = pptctl::optimize(fixture("crop_deck.pptx"), out, opts, error);
    REQUIRE_MESSAGE(report.has_value(), error);
    CHECK(std::none_of(report->passes.begin(), report->passes.end(), [](const auto& p) {
        return p.id == "crop";
    }));
    pptctl::Package b;
    REQUIRE(b.open(out));
    CHECK(to_string(b.read_part(kSlide1)).find(kSlide1Crop) != std::string::npos);
}
