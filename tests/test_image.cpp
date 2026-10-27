// Tests for image header probing and the PNG/JPEG codec (pptctl/image.hpp).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <pptctl/image.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

TEST_CASE("image: sniff_image_format") {
    CHECK(pptctl::sniff_image_format(png(4, 4)) == pptctl::ImageFormat::Png);
    CHECK(pptctl::sniff_image_format(jpeg(4, 4)) == pptctl::ImageFormat::Jpeg);
    CHECK(pptctl::sniff_image_format(Bytes{}) == pptctl::ImageFormat::Unknown);
    Bytes gif;
    push(gif, {'G', 'I', 'F', '8', '9', 'a'});
    CHECK(pptctl::sniff_image_format(gif) == pptctl::ImageFormat::Unknown);
}

// ── codec ─────────────────────────────────────────────────────────────────────

namespace {

using pptctl::Image;

// A smooth gradient with more than 256 distinct colours (so PNG stays truecolour).
Image gradient(std::uint32_t w, std::uint32_t h, std::uint8_t channels) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.pixels.resize(std::size_t{w} * h * channels);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                const std::uint32_t v = c == 3 ? (x * 7 + y * 3) : (x * 255 / w + c * 40 + y);
                img.pixels[i++] = static_cast<std::uint8_t>(v & 0xFF);
            }
        }
    }
    return img;
}

// Flat colour quadrants: at most 4 distinct colours, so PNG goes palette.
Image blocks(std::uint32_t w, std::uint32_t h, std::uint8_t channels) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.pixels.resize(std::size_t{w} * h * channels);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool left = x < w / 2;
            const bool top = y < h / 2;
            for (std::uint32_t c = 0; c < channels; ++c) {
                std::uint8_t v = left ? 200 : 30;
                if (c == 1) {
                    v = top ? 10 : 240;
                }
                if (c == 3) {
                    v = (left && top) ? 0 : 255;  // one fully transparent quadrant
                }
                img.pixels[i++] = v;
            }
        }
    }
    return img;
}

double mean_abs_error(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    double sum = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        sum += std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i]));
    }
    return sum / static_cast<double>(a.pixels.size());
}

bool contains_marker(const Bytes& b, int marker) {
    for (std::size_t i = 0; i + 1 < b.size(); ++i) {
        if (b[i] == std::byte{0xFF} && b[i + 1] == static_cast<std::byte>(marker)) {
            return true;
        }
    }
    return false;
}

bool contains_text(const Bytes& b, std::string_view text) {
    const auto it =
        std::search(b.begin(), b.end(), text.begin(), text.end(), [](std::byte x, char y) {
            return x == static_cast<std::byte>(y);
        });
    return it != b.end();
}

// A minimal ICC profile libpng accepts: a 128-byte header (size, 'acsp'
// magic, display class, RGB data colour space, XYZ PCS, D50 illuminant) and a
// one-entry tag table holding the media white point, then 256 bytes of noise.
// (libpng rejects a profile with no tag data, and one that deflates to fewer
// than ~70 bytes, as "too short".)
Bytes fake_icc() {
    constexpr std::uint32_t kSize = 132 + 12 + 20 + 256;
    Bytes p(kSize, std::byte{0});
    std::uint32_t noise = 12345;
    for (std::size_t i = 164; i < kSize; ++i) {
        noise = noise * 1103515245U + 12345U;
        p[i] = static_cast<std::byte>((noise >> 16) & 0xFF);
    }
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (std::size_t k = 0; k < 4; ++k) {
            p[at + k] = static_cast<std::byte>((v >> (24 - 8 * k)) & 0xFF);
        }
    };
    const auto put4cc = [&](std::size_t at, std::string_view s) {
        for (std::size_t k = 0; k < 4; ++k) {
            p[at + k] = static_cast<std::byte>(s[k]);
        }
    };
    put32(0, kSize);
    put32(8, 0x02100000);  // version 2.1
    put4cc(12, "mntr");
    put4cc(16, "RGB ");
    put4cc(20, "XYZ ");
    put4cc(36, "acsp");
    put32(68, 0x0000F6D6);  // D50 X
    put32(72, 0x00010000);  // D50 Y
    put32(76, 0x0000D32D);  // D50 Z
    put32(128, 1);          // tag count
    put4cc(132, "wtpt");    // tag: signature, offset, size
    put32(136, 144);
    put32(140, 20);
    put4cc(144, "XYZ ");  // XYZType: type, reserved, X, Y, Z
    put32(152, 0x0000F6D6);
    put32(156, 0x00010000);
    put32(160, 0x0000D32D);
    return p;
}

// ── hand-built PNGs (16-bit, APNG) ───────────────────────────────────────────

std::uint32_t crc32(const Bytes& b, std::size_t from, std::size_t to) {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = from; i < to; ++i) {
        c ^= static_cast<std::uint32_t>(b[i]);
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
    }
    return c ^ 0xFFFFFFFFU;
}

void push_chunk(Bytes& b, std::string_view tag, const Bytes& data) {
    push_be32(b, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = b.size();
    for (const char ch : tag) {
        b.push_back(static_cast<std::byte>(ch));
    }
    b.insert(b.end(), data.begin(), data.end());
    push_be32(b, crc32(b, start, b.size()));
}

// zlib stream holding `raw` in one stored (uncompressed) deflate block.
Bytes zlib_stored(const Bytes& raw) {
    Bytes z;
    push(z, {0x78, 0x01, 0x01});
    const auto len = static_cast<std::uint32_t>(raw.size());
    const std::uint32_t nlen = ~len;
    push(z, {static_cast<int>(len & 0xFF), static_cast<int>((len >> 8) & 0xFF)});
    push(z, {static_cast<int>(nlen & 0xFF), static_cast<int>((nlen >> 8) & 0xFF)});
    z.insert(z.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t s = 0;
    for (const auto byte : raw) {
        a = (a + static_cast<std::uint32_t>(byte)) % 65521U;
        s = (s + a) % 65521U;
    }
    push_be32(z, (s << 16) | a);
    return z;
}

struct RawPngSpec {
    std::uint32_t width;
    std::uint32_t height;
    int bit_depth;
    int colour_type;
    std::size_t bytes_per_pixel;
};

// A valid PNG with all-zero pixels; `extra` chunks go between IHDR and IDAT.
Bytes raw_png(const RawPngSpec& spec,
              const std::vector<std::pair<std::string, Bytes>>& extra = {}) {
    Bytes b;
    push(b, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    Bytes ihdr;
    push_be32(ihdr, spec.width);
    push_be32(ihdr, spec.height);
    push(ihdr, {spec.bit_depth, spec.colour_type, 0, 0, 0});
    push_chunk(b, "IHDR", ihdr);
    for (const auto& [tag, data] : extra) {
        push_chunk(b, tag, data);
    }
    const std::size_t row = 1 + std::size_t{spec.width} * spec.bytes_per_pixel;
    push_chunk(b, "IDAT", zlib_stored(Bytes(row * spec.height, std::byte{0})));
    push_chunk(b, "IEND", {});
    return b;
}

// An 8x8 CMYK JPEG (Adobe APP14, transform 0), as Pillow writes it.
constexpr std::array<std::uint8_t, 351> kCmykJpeg = {
    0xFF, 0xD8, 0xFF, 0xEE, 0x00, 0x0E, 0x41, 0x64, 0x6F, 0x62, 0x65, 0x00, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x10, 0x0B, 0x0C, 0x0E, 0x0C, 0x0A, 0x10, 0x0E, 0x0D,
    0x0E, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28, 0x1A, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23, 0x25, 0x1D,
    0x28, 0x3A, 0x33, 0x3D, 0x3C, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5C, 0x4E, 0x40, 0x44, 0x57,
    0x45, 0x37, 0x38, 0x50, 0x6D, 0x51, 0x57, 0x5F, 0x62, 0x67, 0x68, 0x67, 0x3E, 0x4D, 0x71, 0x79,
    0x70, 0x64, 0x78, 0x5C, 0x65, 0x67, 0x63, 0xFF, 0xC0, 0x00, 0x14, 0x08, 0x00, 0x08, 0x00, 0x08,
    0x04, 0x43, 0x11, 0x00, 0x4D, 0x11, 0x00, 0x59, 0x11, 0x00, 0x4B, 0x11, 0x00, 0xFF, 0xC4, 0x00,
    0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4,
    0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13,
    0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15,
    0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4,
    0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2,
    0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
    0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,
    0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x0E, 0x04, 0x43, 0x00, 0x4D, 0x00, 0x59, 0x00,
    0x4B, 0x00, 0x00, 0x3F, 0x00, 0xEE, 0xEB, 0x9A, 0xAE, 0xC6, 0xBD, 0x02, 0xBF, 0xFF, 0xD9};

const std::string_view kExifHeader{"Exif\0\0", 6};

}  // namespace

// ── JPEG ──────────────────────────────────────────────────────────────────────

TEST_CASE("codec: RGB JPEG round-trips with its dimensions and close pixels") {
    const auto src = gradient(64, 48, 3);
    const auto bytes = pptctl::encode_jpeg(src, {.quality = 90, .progressive = false});
    REQUIRE_FALSE(bytes.empty());
    const auto dims = probe(bytes);
    REQUIRE(dims.has_value());
    CHECK(dims->width == 64);
    CHECK(dims->height == 48);

    const auto decoded = pptctl::decode_jpeg(bytes);
    REQUIRE_MESSAGE(decoded.image.has_value(), decoded.reason);
    CHECK(decoded.image->width == 64);
    CHECK(decoded.image->height == 48);
    CHECK(decoded.image->channels == 3);
    CHECK(mean_abs_error(src, *decoded.image) < 8.0);
    CHECK(decoded.image->exif_orientation == 0);
    CHECK(decoded.image->icc_profile.empty());
}

TEST_CASE("codec: JPEG progressive flag selects SOF2 over a baseline frame") {
    const auto src = gradient(32, 32, 3);
    const auto progressive = pptctl::encode_jpeg(src, {.quality = 85, .progressive = true});
    const auto baseline = pptctl::encode_jpeg(src, {.quality = 85, .progressive = false});
    CHECK(contains_marker(progressive, 0xC2));
    CHECK_FALSE(contains_marker(baseline, 0xC2));
    CHECK(pptctl::decode_jpeg(progressive).image.has_value());
}

TEST_CASE("codec: lower JPEG quality yields fewer bytes") {
    const auto src = gradient(128, 128, 3);
    CHECK(pptctl::encode_jpeg(src, {.quality = 40}).size() <
          pptctl::encode_jpeg(src, {.quality = 95}).size());
}

TEST_CASE("codec: grayscale JPEG stays one channel") {
    const auto src = gradient(40, 30, 1);
    const auto decoded = pptctl::decode_jpeg(pptctl::encode_jpeg(src));
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->channels == 1);
    CHECK(mean_abs_error(src, *decoded.image) < 4.0);
}

TEST_CASE("codec: JPEG keeps the EXIF orientation tag and its ICC profile") {
    auto src = gradient(24, 16, 3);
    src.exif_orientation = 6;
    src.icc_profile = fake_icc();
    const auto bytes = pptctl::encode_jpeg(src);
    CHECK(contains_text(bytes, kExifHeader));
    const auto decoded = pptctl::decode_jpeg(bytes);
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->exif_orientation == 6);
    CHECK(decoded.image->icc_profile == src.icc_profile);
    // Orientation is reported, not applied: the stored pixel grid is unchanged.
    CHECK(decoded.image->width == 24);
    CHECK(decoded.image->height == 16);
}

TEST_CASE("codec: JPEG writes no EXIF block for the identity orientation") {
    auto src = gradient(16, 16, 3);
    src.exif_orientation = 1;
    const auto bytes = pptctl::encode_jpeg(src);
    CHECK_FALSE(contains_text(bytes, kExifHeader));
    const auto decoded = pptctl::decode_jpeg(bytes);
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->exif_orientation == 0);
}

TEST_CASE("codec: encode_jpeg refuses alpha and inconsistent images") {
    CHECK(pptctl::encode_jpeg(gradient(8, 8, 4)).empty());
    auto bad = gradient(8, 8, 3);
    bad.pixels.pop_back();
    CHECK(pptctl::encode_jpeg(bad).empty());
}

TEST_CASE("codec: CMYK JPEG is refused, not converted") {
    const auto decoded = pptctl::decode_jpeg(std::as_bytes(std::span(kCmykJpeg)));
    CHECK_FALSE(decoded.image.has_value());
    CHECK(decoded.reason.find("CMYK") != std::string::npos);
}

TEST_CASE("codec: truncated or garbage JPEG fails with a reason") {
    auto bytes = pptctl::encode_jpeg(gradient(64, 64, 3));
    bytes.resize(bytes.size() / 2);
    const auto truncated = pptctl::decode_jpeg(bytes);
    CHECK_FALSE(truncated.image.has_value());
    CHECK_FALSE(truncated.reason.empty());

    const auto garbage = pptctl::decode_jpeg(png(4, 4));
    CHECK_FALSE(garbage.image.has_value());
    CHECK_FALSE(garbage.reason.empty());
}

// ── PNG ───────────────────────────────────────────────────────────────────────

TEST_CASE("codec: truecolour RGBA PNG round-trips losslessly") {
    const auto src = gradient(70, 50, 4);
    const auto bytes = pptctl::encode_png(src);
    REQUIRE(bytes.size() > 26);
    CHECK(static_cast<int>(bytes[25]) == 6);  // IHDR colour type: RGBA
    const auto decoded = pptctl::decode_png(bytes);
    REQUIRE_MESSAGE(decoded.image.has_value(), decoded.reason);
    CHECK(decoded.image->channels == 4);
    CHECK(decoded.image->has_alpha());
    CHECK(decoded.image->pixels == src.pixels);
}

TEST_CASE("codec: PNG gray and gray+alpha keep their layout") {
    for (const std::uint8_t channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto src = gradient(33, 21, channels);
        const auto decoded = pptctl::decode_png(pptctl::encode_png(src));
        REQUIRE(decoded.image.has_value());
        CHECK(decoded.image->channels == channels);
        CHECK(decoded.image->pixels == src.pixels);
    }
}

TEST_CASE("codec: few-colour PNG is written as a palette and decodes identically") {
    const auto rgba = blocks(64, 64, 4);
    const auto bytes = pptctl::encode_png(rgba);
    REQUIRE(bytes.size() > 26);
    CHECK(static_cast<int>(bytes[25]) == 3);  // IHDR colour type: palette
    const auto decoded = pptctl::decode_png(bytes);
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->channels == 4);  // tRNS restores the alpha channel
    CHECK(decoded.image->pixels == rgba.pixels);

    const auto rgb = blocks(64, 64, 3);
    const auto rgb_decoded = pptctl::decode_png(pptctl::encode_png(rgb));
    REQUIRE(rgb_decoded.image.has_value());
    CHECK(rgb_decoded.image->channels == 3);
    CHECK(rgb_decoded.image->pixels == rgb.pixels);
}

TEST_CASE("codec: PNG compresses a flat image to a few hundred bytes") {
    Image flat;
    flat.width = 512;
    flat.height = 512;
    flat.channels = 3;
    flat.pixels.assign(std::size_t{512} * 512 * 3, 128);
    CHECK(pptctl::encode_png(flat).size() < 1024);
}

TEST_CASE("codec: PNG carries iCCP, gAMA and cHRM through a re-encode") {
    auto src = gradient(16, 16, 3);
    src.icc_profile = fake_icc();
    src.png_gamma = 45455;
    src.png_chrm =
        std::array<std::int32_t, 8>{31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
    const auto bytes = pptctl::encode_png(src);
    REQUIRE_FALSE(bytes.empty());
    const auto decoded = pptctl::decode_png(bytes);
    REQUIRE_MESSAGE(decoded.image.has_value(), decoded.reason);
    CHECK(decoded.image->icc_profile == src.icc_profile);
    CHECK(decoded.image->png_gamma == src.png_gamma);
    CHECK(decoded.image->png_chrm == src.png_chrm);
}

TEST_CASE("codec: PNG carries the sRGB intent through a re-encode") {
    auto src = gradient(16, 16, 3);
    src.png_srgb_intent = 0;  // perceptual
    const auto decoded = pptctl::decode_png(pptctl::encode_png(src));
    REQUIRE(decoded.image.has_value());
    CHECK(decoded.image->png_srgb_intent == 0);
}

TEST_CASE("codec: hand-built 8-bit PNG decodes (the test builder is sound)") {
    const auto decoded = pptctl::decode_png(raw_png({5, 3, 8, 2, 3}));
    REQUIRE_MESSAGE(decoded.image.has_value(), decoded.reason);
    CHECK(decoded.image->width == 5);
    CHECK(decoded.image->channels == 3);
}

TEST_CASE("codec: 16-bit PNG is refused, not truncated") {
    const auto decoded = pptctl::decode_png(raw_png({5, 3, 16, 2, 6}));
    CHECK_FALSE(decoded.image.has_value());
    CHECK(decoded.reason.find("16-bit") != std::string::npos);
}

TEST_CASE("codec: animated PNG (acTL) is refused") {
    Bytes actl;
    push_be32(actl, 2);  // frames
    push_be32(actl, 0);  // plays
    const auto decoded = pptctl::decode_png(raw_png({5, 3, 8, 2, 3}, {{"acTL", actl}}));
    CHECK_FALSE(decoded.image.has_value());
    CHECK(decoded.reason.find("animated") != std::string::npos);
}

TEST_CASE("codec: truncated or garbage PNG fails with a reason") {
    auto bytes = pptctl::encode_png(gradient(64, 64, 3));
    bytes.resize(bytes.size() / 2);
    const auto truncated = pptctl::decode_png(bytes);
    CHECK_FALSE(truncated.image.has_value());
    CHECK_FALSE(truncated.reason.empty());

    const auto garbage = pptctl::decode_png(jpeg(4, 4));
    CHECK_FALSE(garbage.image.has_value());
    CHECK_FALSE(garbage.reason.empty());
}

TEST_CASE("codec: encode_png refuses inconsistent images") {
    auto bad = gradient(8, 8, 4);
    bad.pixels.pop_back();
    CHECK(pptctl::encode_png(bad).empty());
    bad = gradient(8, 8, 3);
    bad.channels = 5;
    CHECK(pptctl::encode_png(bad).empty());
}
