// Tests for image header probing (pptctl/image.hpp).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <pptctl/image.hpp>
#include <span>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void push(Bytes& out, std::initializer_list<int> values) {
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
}

void push_be16(Bytes& out, std::uint32_t v) {
    push(out, {static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF)});
}

void push_be32(Bytes& out, std::uint32_t v) {
    push(out,
         {static_cast<int>((v >> 24) & 0xFF),
          static_cast<int>((v >> 16) & 0xFF),
          static_cast<int>((v >> 8) & 0xFF),
          static_cast<int>(v & 0xFF)});
}

// PNG: 8-byte signature, then the IHDR chunk (length, tag, 13-byte payload, CRC).
// The CRC is not validated by the probe, so a zero placeholder is fine.
Bytes png(std::uint32_t width, std::uint32_t height) {
    Bytes b;
    push(b, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    push_be32(b, 13);
    push(b, {'I', 'H', 'D', 'R'});
    push_be32(b, width);
    push_be32(b, height);
    push(b, {8, 2, 0, 0, 0});  // bit depth, colour type, compression, filter, interlace
    push_be32(b, 0);           // CRC placeholder
    return b;
}

// A JPEG segment: marker byte, 2-byte length (counting itself), payload.
void push_segment(Bytes& b, int marker, const Bytes& payload) {
    push(b, {0xFF, marker});
    push_be16(b, static_cast<std::uint32_t>(payload.size() + 2));
    b.insert(b.end(), payload.begin(), payload.end());
}

// SOF payload: precision, height, width, component count, one 3-byte component.
Bytes sof_payload(std::uint32_t width, std::uint32_t height) {
    Bytes p;
    push(p, {8});
    push_be16(p, height);
    push_be16(p, width);
    push(p, {1, 0x01, 0x11, 0x00});
    return p;
}

// JPEG: SOI, then the given leading segments, then SOF<kind>, then SOS + EOI.
Bytes jpeg(std::uint32_t width,
           std::uint32_t height,
           int sof_marker = 0xC0,
           const std::vector<std::pair<int, Bytes>>& leading = {{0xE0, Bytes(14)}}) {
    Bytes b;
    push(b, {0xFF, 0xD8});
    for (const auto& [marker, payload] : leading) {
        push_segment(b, marker, payload);
    }
    push_segment(b, sof_marker, sof_payload(width, height));
    push_segment(b, 0xDA, Bytes(6));
    push(b, {0xFF, 0xD9});
    return b;
}

std::optional<pptctl::ImageDimensions> probe(const Bytes& b) {
    return pptctl::probe_image_dimensions(std::span<const std::byte>(b));
}

}  // namespace

// ── PNG ───────────────────────────────────────────────────────────────────────

TEST_CASE("image: PNG IHDR yields width and height") {
    const auto d = probe(png(1600, 1200));
    REQUIRE(d.has_value());
    CHECK(d->width == 1600);
    CHECK(d->height == 1200);
}

TEST_CASE("image: PNG with large dimensions reads all four bytes") {
    const auto d = probe(png(70'000, 65'537));
    REQUIRE(d.has_value());
    CHECK(d->width == 70'000);
    CHECK(d->height == 65'537);
}

TEST_CASE("image: PNG truncated before IHDR payload — nullopt") {
    auto b = png(64, 48);
    b.resize(20);
    CHECK_FALSE(probe(b).has_value());
}

TEST_CASE("image: PNG with zero width — nullopt") {
    CHECK_FALSE(probe(png(0, 48)).has_value());
}

TEST_CASE("image: PNG signature but first chunk is not IHDR — nullopt") {
    auto b = png(64, 48);
    b[12] = static_cast<std::byte>('X');
    CHECK_FALSE(probe(b).has_value());
}

// ── JPEG ──────────────────────────────────────────────────────────────────────

TEST_CASE("image: baseline JPEG (SOF0) after APP0 yields dimensions") {
    const auto d = probe(jpeg(4032, 3024));
    REQUIRE(d.has_value());
    CHECK(d->width == 4032);
    CHECK(d->height == 3024);
}

TEST_CASE("image: progressive JPEG (SOF2) yields dimensions") {
    const auto d = probe(jpeg(800, 600, 0xC2));
    REQUIRE(d.has_value());
    CHECK(d->width == 800);
    CHECK(d->height == 600);
}

TEST_CASE("image: JPEG with large APP1 (EXIF) and DQT before SOF") {
    Bytes exif(3000, std::byte{0x42});
    const auto d = probe(jpeg(1920, 1080, 0xC0, {{0xE1, exif}, {0xDB, Bytes(65)}}));
    REQUIRE(d.has_value());
    CHECK(d->width == 1920);
    CHECK(d->height == 1080);
}

TEST_CASE("image: JPEG with 0xFF fill bytes before a marker") {
    Bytes b;
    push(b, {0xFF, 0xD8, 0xFF, 0xFF, 0xFF});
    push_segment(b, 0xC0, sof_payload(320, 240));
    const auto d = probe(b);
    REQUIRE(d.has_value());
    CHECK(d->width == 320);
    CHECK(d->height == 240);
}

TEST_CASE("image: JPEG with standalone RSTn/TEM markers (no length) before SOF") {
    Bytes b;
    push(b, {0xFF, 0xD8, 0xFF, 0x01, 0xFF, 0xD0});
    push_segment(b, 0xC1, sof_payload(100, 50));
    const auto d = probe(b);
    REQUIRE(d.has_value());
    CHECK(d->width == 100);
    CHECK(d->height == 50);
}

TEST_CASE("image: JPEG DHT (0xC4) and DAC (0xCC) are not treated as SOF") {
    // Their payloads would decode to garbage dimensions if mistaken for SOF.
    Bytes dht;
    push(dht, {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00});
    const auto d = probe(jpeg(640, 480, 0xC0, {{0xC4, dht}, {0xCC, dht}}));
    REQUIRE(d.has_value());
    CHECK(d->width == 640);
    CHECK(d->height == 480);
}

TEST_CASE("image: JPEG truncated inside the SOF segment — nullopt") {
    auto b = jpeg(640, 480);
    b.resize(b.size() - 14);  // SOS + EOI are 12 bytes; cut 2 more into the SOF payload
    CHECK_FALSE(probe(b).has_value());
}

TEST_CASE("image: JPEG with SOS before any SOF — nullopt") {
    Bytes b;
    push(b, {0xFF, 0xD8});
    push_segment(b, 0xDA, Bytes(6));
    push_segment(b, 0xC0, sof_payload(10, 10));
    CHECK_FALSE(probe(b).has_value());
}

TEST_CASE("image: JPEG with zero height in SOF — nullopt") {
    CHECK_FALSE(probe(jpeg(640, 0)).has_value());
}

TEST_CASE("image: JPEG whose segment length overruns the buffer — nullopt") {
    Bytes b;
    push(b, {0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xFF, 0x00});
    CHECK_FALSE(probe(b).has_value());
}

// ── other input ───────────────────────────────────────────────────────────────

TEST_CASE("image: empty input — nullopt") {
    CHECK_FALSE(probe(Bytes{}).has_value());
}

TEST_CASE("image: GIF header — nullopt (unsupported format)") {
    Bytes b;
    push(b, {'G', 'I', 'F', '8', '9', 'a', 0x10, 0x00, 0x10, 0x00, 0, 0, 0});
    CHECK_FALSE(probe(b).has_value());
}

TEST_CASE("image: XML/SVG text — nullopt") {
    Bytes b;
    push(b, {'<', '?', 'x', 'm', 'l', ' ', 'v', 'e', 'r', 's', 'i', 'o', 'n', '=', '"', '1'});
    push(b, {'.', '0', '"', '?', '>', '<', 's', 'v', 'g', '/', '>'});
    CHECK_FALSE(probe(b).has_value());
}
