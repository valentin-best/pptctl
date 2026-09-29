// Image header probing — see include/pptctl/image.hpp.
//
// Both decoders are deliberately minimal: they read just enough of the
// container to find the dimensions and bail out (nullopt) at the first sign
// of truncation or malformation. No CRC, no entropy-coded data, no allocation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pptctl/image.hpp>
#include <span>

namespace pptctl {

namespace {

using Bytes = std::span<const std::byte>;

std::uint8_t u8(Bytes b, std::size_t i) noexcept {
    return static_cast<std::uint8_t>(b[i]);
}

std::uint32_t be16(Bytes b, std::size_t i) noexcept {
    return (static_cast<std::uint32_t>(u8(b, i)) << 8) | u8(b, i + 1);
}

std::uint32_t be32(Bytes b, std::size_t i) noexcept {
    return (be16(b, i) << 16) | be16(b, i + 2);
}

std::optional<ImageDimensions> make_dims(std::uint32_t w, std::uint32_t h) noexcept {
    if (w == 0 || h == 0) {
        return std::nullopt;
    }
    return ImageDimensions{w, h};
}

// ── PNG ───────────────────────────────────────────────────────────────────────
//
// Layout: 8-byte signature, then chunks of [len:4][tag:4][data:len][crc:4].
// The spec requires IHDR to be the first chunk, and its first 8 data bytes are
// width and height as big-endian u32.

constexpr std::size_t kPngHeaderBytes = 8 + 4 + 4 + 8;  // signature + len + "IHDR" + w + h
constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

bool is_png(Bytes b) noexcept {
    if (b.size() < kPngSignature.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPngSignature.size(); ++i) {
        if (u8(b, i) != kPngSignature[i]) {
            return false;
        }
    }
    return true;
}

std::optional<ImageDimensions> probe_png(Bytes b) noexcept {
    if (b.size() < kPngHeaderBytes) {
        return std::nullopt;
    }
    if (u8(b, 12) != 'I' || u8(b, 13) != 'H' || u8(b, 14) != 'D' || u8(b, 15) != 'R') {
        return std::nullopt;
    }
    return make_dims(be32(b, 16), be32(b, 20));
}

// ── JPEG ──────────────────────────────────────────────────────────────────────
//
// Layout: SOI (FF D8), then marker segments [FF][marker][len:2][data:len-2]
// until a Start-Of-Frame marker, whose payload is [precision:1][height:2]
// [width:2][components...]. Markers without a length field: TEM (01), RSTn
// (D0–D7), SOI (D8), EOI (D9). Any number of FF fill bytes may precede a
// marker. The dimensions must appear before SOS (DA), after which the stream
// is entropy-coded and no longer segment-structured.

bool is_jpeg(Bytes b) noexcept {
    return b.size() >= 2 && u8(b, 0) == 0xFF && u8(b, 1) == 0xD8;
}

// SOF0–SOF15 are C0–CF except C4 (DHT), C8 (JPG extension) and CC (DAC).
bool is_sof(std::uint8_t m) noexcept {
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

bool is_standalone(std::uint8_t m) noexcept {
    return m == 0x01 || (m >= 0xD0 && m <= 0xD9);
}

std::optional<ImageDimensions> probe_jpeg(Bytes b) noexcept {
    std::size_t i = 2;  // past SOI
    while (i + 1 < b.size()) {
        if (u8(b, i) != 0xFF) {
            return std::nullopt;  // lost segment sync
        }
        const std::uint8_t marker = u8(b, i + 1);
        if (marker == 0xFF) {
            ++i;  // fill byte
            continue;
        }
        i += 2;
        if (is_standalone(marker)) {
            continue;
        }
        if (i + 2 > b.size()) {
            return std::nullopt;
        }
        const std::size_t len = be16(b, i);
        if (len < 2 || i + len > b.size()) {
            return std::nullopt;
        }
        if (is_sof(marker)) {
            if (len < 7) {
                return std::nullopt;
            }
            return make_dims(be16(b, i + 5), be16(b, i + 3));
        }
        if (marker == 0xDA) {
            return std::nullopt;  // scan data without a preceding frame header
        }
        i += len;
    }
    return std::nullopt;
}

}  // namespace

std::optional<ImageDimensions> probe_image_dimensions(Bytes bytes) noexcept {
    if (is_png(bytes)) {
        return probe_png(bytes);
    }
    if (is_jpeg(bytes)) {
        return probe_jpeg(bytes);
    }
    return std::nullopt;
}

}  // namespace pptctl
