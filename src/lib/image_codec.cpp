// PNG/JPEG codec — see include/pptctl/image.hpp.
//
// Both libraries report fatal errors by longjmp. That is only safe in C++ when
// the jump skips no frame owning an object with a non-trivial destructor, so
// every libjpeg/libpng call runs inside guarded(): the setjmp lives there, the
// body is a lambda that touches only trivially destructible locals and the
// caller's objects, and everything that allocates (pixel buffers, row tables,
// output vectors) is set up in the caller between guarded phases. A fatal
// error therefore unwinds through C frames and the lambda only. Warnings are
// counted, and a decode or encode that raised any is treated as a failure:
// libjpeg warns about corrupt or truncated data and then carries on with grey
// fill, which a re-encode would bake in.

// jpeglib.h uses FILE and size_t without including their headers.
// clang-format off
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
// clang-format on

#include <png.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <pptctl/image.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pptctl {

namespace {

using Bytes = std::span<const std::byte>;

std::size_t pixel_count(std::uint32_t w, std::uint32_t h) noexcept {
    return static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
}

bool too_large(std::uint32_t w, std::uint32_t h) noexcept {
    return static_cast<std::uint64_t>(w) * h > kMaxDecodePixels;
}

bool consistent(const Image& img) noexcept {
    return img.width > 0 && img.height > 0 && img.channels >= 1 && img.channels <= 4 &&
           !too_large(img.width, img.height) &&
           img.pixels.size() == pixel_count(img.width, img.height) * img.channels;
}

DecodeResult failure(std::string reason) {
    return {std::nullopt, std::move(reason)};
}

std::uint32_t be16(Bytes b, std::size_t i) noexcept {
    return (static_cast<std::uint32_t>(b[i]) << 8) | static_cast<std::uint32_t>(b[i + 1]);
}

std::uint32_t be32(Bytes b, std::size_t i) noexcept {
    return (be16(b, i) << 16) | be16(b, i + 2);
}

// ── JPEG ──────────────────────────────────────────────────────────────────────

struct JpegErrors {
    jpeg_error_mgr mgr{};  // first member: libjpeg hands back a pointer to it
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

[[noreturn]] void jpeg_on_error(j_common_ptr cinfo) {
    auto* errors = reinterpret_cast<JpegErrors*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, errors->message.data());
    std::longjmp(errors->jump, 1);
}

// The default emit_message counts warnings in num_warnings and routes text to
// output_message; this keeps the count and drops the text (no stderr noise).
void jpeg_on_output(j_common_ptr /*cinfo*/) {}

void install(JpegErrors& errors) {
    jpeg_std_error(&errors.mgr);
    errors.mgr.error_exit = jpeg_on_error;
    errors.mgr.output_message = jpeg_on_output;
}

// Run `body` under libjpeg's error handling: false when it raised a fatal error.
template <typename F>
bool guarded(JpegErrors& errors, const F& body) {
    // NOLINTNEXTLINE(cert-err52-cpp): libjpeg's only error channel; see the file comment.
    if (setjmp(errors.jump) != 0) {
        return false;
    }
    body();
    return true;
}

std::string jpeg_reason(const JpegErrors& errors) {
    return "corrupt JPEG: " + std::string(errors.message.data());
}

// Destroys a (de)compress struct on scope exit. jpeg_destroy is a no-op on a
// zero-initialised struct, so this is safe even when creation failed.
template <typename T>
struct JpegStructGuard {
    T& cinfo;
    explicit JpegStructGuard(T& c) : cinfo(c) {}
    JpegStructGuard(const JpegStructGuard&) = delete;
    JpegStructGuard& operator=(const JpegStructGuard&) = delete;
    JpegStructGuard(JpegStructGuard&&) = delete;
    JpegStructGuard& operator=(JpegStructGuard&&) = delete;
    ~JpegStructGuard() {
        jpeg_destroy(reinterpret_cast<j_common_ptr>(&cinfo));
    }
};

constexpr std::string_view kExifHeader{"Exif\0\0", 6};
constexpr std::uint16_t kOrientationTag = 0x0112;

// The Orientation tag (1-8) from an APP1 Exif payload; 0 when absent or invalid.
std::uint16_t exif_orientation(Bytes app1) noexcept {
    if (app1.size() < kExifHeader.size() + 8 ||
        !std::equal(kExifHeader.begin(), kExifHeader.end(), app1.begin(), [](char c, std::byte b) {
            return static_cast<std::byte>(c) == b;
        })) {
        return 0;
    }
    const Bytes tiff = app1.subspan(kExifHeader.size());
    const bool big = tiff[0] == std::byte{'M'} && tiff[1] == std::byte{'M'};
    const bool little = tiff[0] == std::byte{'I'} && tiff[1] == std::byte{'I'};
    if (!big && !little) {
        return 0;
    }
    const auto u16 = [&](std::size_t i) -> std::uint32_t {
        return big ? be16(tiff, i)
                   : (static_cast<std::uint32_t>(tiff[i + 1]) << 8) |
                         static_cast<std::uint32_t>(tiff[i]);
    };
    const auto u32 = [&](std::size_t i) -> std::uint32_t {
        return big ? (u16(i) << 16) | u16(i + 2) : (u16(i + 2) << 16) | u16(i);
    };
    if (u16(2) != 42) {
        return 0;
    }
    const std::size_t ifd = u32(4);
    if (ifd + 2 > tiff.size()) {
        return 0;
    }
    const std::size_t count = u16(ifd);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t entry = ifd + 2 + 12 * k;
        if (entry + 12 > tiff.size()) {
            return 0;
        }
        constexpr std::uint32_t kShort = 3;
        if (u16(entry) == kOrientationTag && u16(entry + 2) == kShort && u32(entry + 4) == 1) {
            const std::uint32_t v = u16(entry + 8);
            return v >= 1 && v <= 8 ? static_cast<std::uint16_t>(v) : 0;
        }
    }
    return 0;
}

// An APP1 Exif payload holding only IFD0 with the Orientation tag.
std::vector<JOCTET> exif_with_orientation(std::uint16_t orientation) {
    std::vector<JOCTET> out(kExifHeader.begin(), kExifHeader.end());
    const std::array<JOCTET, 26> tiff = {
        'M',
        'M',
        0,
        42,
        0,
        0,
        0,
        8,  // big-endian TIFF header, IFD0 at 8
        0,
        1,  // one entry
        0x01,
        0x12,
        0,
        3,
        0,
        0,
        0,
        1,                                      // Orientation, SHORT, count 1
        static_cast<JOCTET>(orientation >> 8),  // value, left-justified
        static_cast<JOCTET>(orientation & 0xFF),
        0,
        0,  //
        0,
        0,
        0,
        0};  // no next IFD
    out.insert(out.end(), tiff.begin(), tiff.end());
    return out;
}

DecodeResult decode_jpeg_impl(Bytes bytes) {
    if (sniff_image_format(bytes) != ImageFormat::Jpeg) {
        return failure("not a JPEG");
    }
    jpeg_decompress_struct cinfo{};
    JpegErrors errors;
    install(errors);
    cinfo.err = &errors.mgr;
    const JpegStructGuard<jpeg_decompress_struct> guard{cinfo};

    JOCTET* icc = nullptr;
    unsigned int icc_len = 0;
    const bool header_ok = guarded(errors, [&] {
        jpeg_CreateDecompress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
        jpeg_mem_src(&cinfo,
                     reinterpret_cast<const unsigned char*>(bytes.data()),
                     static_cast<unsigned long>(bytes.size()));
        jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
        jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
        (void)jpeg_read_header(&cinfo, TRUE);
        (void)jpeg_read_icc_profile(&cinfo, &icc, &icc_len);
    });
    Image img;
    if (icc != nullptr) {
        const auto* first = reinterpret_cast<const std::byte*>(icc);
        img.icc_profile.assign(first, first + icc_len);
        std::free(icc);  // NOLINT(cppcoreguidelines-no-malloc): libjpeg malloc'd it
    }
    if (!header_ok) {
        return failure(jpeg_reason(errors));
    }

    if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
        return failure("CMYK JPEG");
    }
    if (cinfo.data_precision != 8) {
        return failure(std::to_string(cinfo.data_precision) + "-bit JPEG");
    }
    switch (cinfo.jpeg_color_space) {
        case JCS_GRAYSCALE:
            cinfo.out_color_space = JCS_GRAYSCALE;
            break;
        case JCS_YCbCr:
        case JCS_RGB:
            cinfo.out_color_space = JCS_RGB;
            break;
        default:
            return failure("JPEG with an unsupported colour space");
    }
    if (too_large(cinfo.image_width, cinfo.image_height)) {
        return failure("JPEG too large to decode");
    }
    for (auto* m = cinfo.marker_list; m != nullptr; m = m->next) {
        if (m->marker == JPEG_APP0 + 1 && img.exif_orientation == 0) {
            img.exif_orientation =
                exif_orientation(std::as_bytes(std::span<const JOCTET>(m->data, m->data_length)));
        }
    }

    if (!guarded(errors, [&] { (void)jpeg_start_decompress(&cinfo); })) {
        return failure(jpeg_reason(errors));
    }
    img.width = cinfo.output_width;
    img.height = cinfo.output_height;
    img.channels = static_cast<std::uint8_t>(cinfo.output_components);
    img.pixels.resize(pixel_count(img.width, img.height) * img.channels);
    const std::size_t stride = std::size_t{img.width} * img.channels;
    std::uint8_t* const base = img.pixels.data();
    if (!guarded(errors, [&] {
            while (cinfo.output_scanline < cinfo.output_height) {
                JSAMPROW row = base + stride * cinfo.output_scanline;
                (void)jpeg_read_scanlines(&cinfo, &row, 1);
            }
            (void)jpeg_finish_decompress(&cinfo);
        })) {
        return failure(jpeg_reason(errors));
    }
    if (errors.mgr.num_warnings > 0) {
        std::array<char, JMSG_LENGTH_MAX> text{};
        (*errors.mgr.format_message)(reinterpret_cast<j_common_ptr>(&cinfo), text.data());
        return failure("corrupt JPEG: " + std::string(text.data()));
    }
    return {std::move(img), {}};
}

std::vector<std::byte> encode_jpeg_impl(const Image& img, const JpegEncodeOptions& opts) {
    if (!consistent(img) || (img.channels != 1 && img.channels != 3)) {
        return {};
    }
    const auto exif = img.exif_orientation >= 2 && img.exif_orientation <= 8
                          ? exif_with_orientation(img.exif_orientation)
                          : std::vector<JOCTET>{};
    jpeg_compress_struct cinfo{};
    JpegErrors errors;
    install(errors);
    cinfo.err = &errors.mgr;
    const JpegStructGuard<jpeg_compress_struct> guard{cinfo};

    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    const std::size_t stride = std::size_t{img.width} * img.channels;
    const std::uint8_t* const base = img.pixels.data();
    const bool ok = guarded(errors, [&] {
        jpeg_CreateCompress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
        jpeg_mem_dest(&cinfo, &out, &out_size);
        cinfo.image_width = img.width;
        cinfo.image_height = img.height;
        cinfo.input_components = img.channels;
        cinfo.in_color_space = img.channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, std::clamp(opts.quality, 1, 100), TRUE);
        cinfo.optimize_coding = TRUE;
        if (opts.progressive) {
            jpeg_simple_progression(&cinfo);
        }
        jpeg_start_compress(&cinfo, TRUE);
        if (!exif.empty()) {
            jpeg_write_marker(
                &cinfo, JPEG_APP0 + 1, exif.data(), static_cast<unsigned int>(exif.size()));
        }
        if (!img.icc_profile.empty()) {
            jpeg_write_icc_profile(&cinfo,
                                   reinterpret_cast<const JOCTET*>(img.icc_profile.data()),
                                   static_cast<unsigned int>(img.icc_profile.size()));
        }
        while (cinfo.next_scanline < cinfo.image_height) {
            // libjpeg's API is not const-correct; it only reads the row.
            auto* row = const_cast<std::uint8_t*>(base + stride * cinfo.next_scanline);
            (void)jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
    });
    std::vector<std::byte> result;
    if (ok && errors.mgr.num_warnings == 0 && out != nullptr) {
        const auto* first = reinterpret_cast<const std::byte*>(out);
        result.assign(first, first + out_size);
    }
    std::free(out);  // NOLINT(cppcoreguidelines-no-malloc): jpeg_mem_dest malloc'd it
    return result;
}

// ── PNG ───────────────────────────────────────────────────────────────────────

// Fixed storage: the callbacks run inside libpng's C frames and must not throw.
// Keeps the first error or warning text.
struct PngErrors {
    std::array<char, 200> message{};
    int warnings = 0;

    void note(png_const_charp text) noexcept {
        if (message[0] != '\0') {
            return;
        }
        const std::size_t n = std::min(std::strlen(text), message.size() - 1);
        std::memcpy(message.data(), text, n);
        message[n] = '\0';
    }

    [[nodiscard]] std::string text() const {
        return {message.data()};
    }
};

void png_on_error(png_structp png, png_const_charp message) {
    static_cast<PngErrors*>(png_get_error_ptr(png))->note(message);
    png_longjmp(png, 1);
}

void png_on_warning(png_structp png, png_const_charp message) {
    auto* errors = static_cast<PngErrors*>(png_get_error_ptr(png));
    errors->note(message);
    ++errors->warnings;
}

template <typename F>
bool guarded(png_structp png, const F& body) {
    // NOLINTNEXTLINE(cert-err52-cpp): libpng's only error channel; see the file comment.
    if (setjmp(png_jmpbuf(png)) != 0) {
        return false;
    }
    body();
    return true;
}

struct PngReader {
    Bytes bytes;
    std::size_t pos = 0;
};

void png_read_bytes(png_structp png, png_bytep out, std::size_t length) {
    auto* reader = static_cast<PngReader*>(png_get_io_ptr(png));
    if (length > reader->bytes.size() - reader->pos) {
        png_error(png, "unexpected end of PNG data");
    }
    std::memcpy(out, reader->bytes.data() + reader->pos, length);
    reader->pos += length;
}

// NOLINTNEXTLINE(readability-non-const-parameter): libpng's png_rw_ptr signature
void png_write_bytes(png_structp png, png_bytep data, std::size_t length) {
    auto* out = static_cast<std::vector<std::byte>*>(png_get_io_ptr(png));
    try {
        const auto* first = reinterpret_cast<const std::byte*>(data);
        out->insert(out->end(), first, first + length);
    } catch (const std::exception&) {
        png_error(png, "out of memory");
    }
}

void png_flush_bytes(png_structp /*png*/) {}

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

struct ChunkScan {
    bool truncated = false;
    bool animated = false;  // acTL: APNG
    bool exif = false;      // eXIf: may carry an orientation some viewers apply
    bool cicp = false;      // cICP: coding-independent (HDR) colour signalling
    bool iccp = false;
};

// Walk the chunk list up to the first IDAT without decoding anything.
ChunkScan scan_chunks(Bytes b) noexcept {
    ChunkScan scan;
    std::size_t i = kPngSignature.size();
    while (true) {
        if (i + 8 > b.size()) {
            scan.truncated = true;
            return scan;
        }
        const std::size_t len = be32(b, i);
        const auto tag = [&](std::string_view name) {
            for (std::size_t k = 0; k < 4; ++k) {
                if (b[i + 4 + k] != static_cast<std::byte>(name[k])) {
                    return false;
                }
            }
            return true;
        };
        if (tag("IDAT")) {
            return scan;
        }
        scan.animated = scan.animated || tag("acTL");
        scan.exif = scan.exif || tag("eXIf");
        scan.cicp = scan.cicp || tag("cICP");
        scan.iccp = scan.iccp || tag("iCCP");
        if (len > b.size() - i - 8 || b.size() - i - 8 - len < 4) {
            scan.truncated = true;
            return scan;
        }
        i += 8 + len + 4;
    }
}

struct PngReadGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    PngReadGuard() = default;
    PngReadGuard(const PngReadGuard&) = delete;
    PngReadGuard& operator=(const PngReadGuard&) = delete;
    PngReadGuard(PngReadGuard&&) = delete;
    PngReadGuard& operator=(PngReadGuard&&) = delete;
    ~PngReadGuard() {
        png_destroy_read_struct(&png, &info, nullptr);
    }
};

DecodeResult decode_png_impl(Bytes bytes) {
    if (sniff_image_format(bytes) != ImageFormat::Png) {
        return failure("not a PNG");
    }
    const ChunkScan scan = scan_chunks(bytes);
    if (scan.truncated) {
        return failure("corrupt PNG: truncated before image data");
    }
    if (scan.animated) {
        return failure("animated PNG");
    }
    if (scan.exif) {
        return failure("PNG with eXIf metadata");
    }
    if (scan.cicp) {
        return failure("PNG with cICP colour signalling");
    }

    PngErrors errors;
    PngReader reader{bytes};
    PngReadGuard g;
    g.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &errors, png_on_error, png_on_warning);
    if (g.png == nullptr) {
        return failure("libpng initialisation failed");
    }
    g.info = png_create_info_struct(g.png);
    if (g.info == nullptr) {
        return failure("libpng initialisation failed");
    }

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int colour_type = 0;
    bool has_trns = false;
    png_bytep icc = nullptr;
    png_uint_32 icc_len = 0;
    if (!guarded(g.png, [&] {
            png_set_read_fn(g.png, &reader, png_read_bytes);
            png_set_option(g.png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);
            png_read_info(g.png, g.info);
            int interlace = 0;
            png_get_IHDR(g.png,
                         g.info,
                         &width,
                         &height,
                         &bit_depth,
                         &colour_type,
                         &interlace,
                         nullptr,
                         nullptr);
            has_trns = png_get_valid(g.png, g.info, PNG_INFO_tRNS) != 0;
            png_charp name = nullptr;
            int compression = 0;
            (void)png_get_iCCP(g.png, g.info, &name, &compression, &icc, &icc_len);
        })) {
        return failure("corrupt PNG: " + errors.text());
    }
    if (bit_depth > 8) {
        return failure("16-bit PNG");
    }
    if (too_large(width, height)) {
        return failure("PNG too large to decode");
    }
    if (scan.iccp && icc == nullptr) {
        return failure("PNG with an unreadable iCCP profile: " + errors.text());
    }

    Image img;
    img.width = width;
    img.height = height;
    if (icc != nullptr) {
        const auto* first = reinterpret_cast<const std::byte*>(icc);
        img.icc_profile.assign(first, first + icc_len);
    }
    if (int intent = 0; png_get_sRGB(g.png, g.info, &intent) != 0) {
        img.png_srgb_intent = intent;
    }
    if (png_fixed_point gamma = 0; png_get_gAMA_fixed(g.png, g.info, &gamma) != 0) {
        img.png_gamma = gamma;
    }
    std::array<png_fixed_point, 8> c{};
    if (png_get_cHRM_fixed(
            g.png, g.info, c.data(), &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]) != 0) {
        img.png_chrm = std::array<std::int32_t, 8>{c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]};
    }

    std::size_t rowbytes = 0;
    if (!guarded(g.png, [&] {
            if (colour_type == PNG_COLOR_TYPE_PALETTE) {
                png_set_palette_to_rgb(g.png);
            }
            if (colour_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
                png_set_expand_gray_1_2_4_to_8(g.png);
            }
            if (has_trns) {
                png_set_tRNS_to_alpha(g.png);
            }
            (void)png_set_interlace_handling(g.png);
            png_read_update_info(g.png, g.info);
            img.channels = png_get_channels(g.png, g.info);
            rowbytes = png_get_rowbytes(g.png, g.info);
        })) {
        return failure("corrupt PNG: " + errors.text());
    }
    if (img.channels < 1 || img.channels > 4 || rowbytes != std::size_t{width} * img.channels) {
        return failure("PNG with an unsupported pixel layout");
    }

    img.pixels.resize(pixel_count(width, height) * img.channels);
    std::vector<png_bytep> rows(height);
    for (std::size_t y = 0; y < rows.size(); ++y) {
        rows[y] = img.pixels.data() + y * rowbytes;
    }
    if (!guarded(g.png, [&] {
            png_read_image(g.png, rows.data());
            png_read_end(g.png, nullptr);
        })) {
        return failure("corrupt PNG: " + errors.text());
    }
    if (errors.warnings > 0) {
        return failure("PNG decoded with warnings: " + errors.text());
    }
    return {std::move(img), {}};
}

struct PngWriteGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    PngWriteGuard() = default;
    PngWriteGuard(const PngWriteGuard&) = delete;
    PngWriteGuard& operator=(const PngWriteGuard&) = delete;
    PngWriteGuard(PngWriteGuard&&) = delete;
    PngWriteGuard& operator=(PngWriteGuard&&) = delete;
    ~PngWriteGuard() {
        png_destroy_write_struct(&png, &info);
    }
};

// A lossless palette form of an RGB(A) image: at most 256 colours, entries
// with alpha first so tRNS stays short. Empty when there are more colours.
struct Palette {
    std::vector<png_color> colours;
    std::vector<png_byte> alpha;    // tRNS entries; empty when all opaque
    std::vector<png_byte> indices;  // one per pixel
    int bit_depth = 8;
};

std::optional<Palette> make_palette(const Image& img) {
    const std::size_t n = pixel_count(img.width, img.height);
    const bool rgba = img.channels == 4;
    const auto key_at = [&](std::size_t p) {
        const std::uint8_t* px = img.pixels.data() + p * img.channels;
        const std::uint32_t a = rgba ? px[3] : 0xFFU;
        return (a << 24) | (std::uint32_t{px[0]} << 16) | (std::uint32_t{px[1]} << 8) | px[2];
    };
    std::vector<std::uint32_t> keys;
    {
        std::unordered_map<std::uint32_t, bool> seen;
        for (std::size_t p = 0; p < n; ++p) {
            if (seen.emplace(key_at(p), true).second) {
                keys.push_back(key_at(p));
                if (keys.size() > 256) {
                    return std::nullopt;
                }
            }
        }
    }
    // Alpha is the key's top byte, so ascending keys put translucent entries
    // first and opaque ones last.
    std::sort(keys.begin(), keys.end());
    Palette pal;
    std::unordered_map<std::uint32_t, png_byte> index;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const std::uint32_t key = keys[k];
        pal.colours.push_back({static_cast<png_byte>((key >> 16) & 0xFF),
                               static_cast<png_byte>((key >> 8) & 0xFF),
                               static_cast<png_byte>(key & 0xFF)});
        if ((key >> 24) != 0xFF) {
            pal.alpha.push_back(static_cast<png_byte>(key >> 24));
        }
        index.emplace(key, static_cast<png_byte>(k));
    }
    pal.indices.resize(n);
    for (std::size_t p = 0; p < n; ++p) {
        pal.indices[p] = index.at(key_at(p));
    }
    // The smallest bit depth that indexes every entry.
    pal.bit_depth = 1;
    while ((std::size_t{1} << pal.bit_depth) < keys.size()) {
        pal.bit_depth *= 2;
    }
    return pal;
}

std::vector<std::byte> encode_png_impl(const Image& img) {
    if (!consistent(img)) {
        return {};
    }
    std::optional<Palette> pal;
    if (img.channels >= 3) {
        pal = make_palette(img);
    }
    int colour_type = PNG_COLOR_TYPE_GRAY;
    switch (img.channels) {
        case 2:
            colour_type = PNG_COLOR_TYPE_GRAY_ALPHA;
            break;
        case 3:
            colour_type = PNG_COLOR_TYPE_RGB;
            break;
        case 4:
            colour_type = PNG_COLOR_TYPE_RGBA;
            break;
        default:
            break;
    }
    const std::uint8_t* const base = pal ? pal->indices.data() : img.pixels.data();
    const std::size_t stride = std::size_t{img.width} * (pal ? 1U : img.channels);
    std::vector<png_bytep> rows(img.height);
    for (std::size_t y = 0; y < rows.size(); ++y) {
        // libpng's API is not const-correct; it only reads the rows.
        rows[y] = const_cast<png_bytep>(base + y * stride);
    }

    std::vector<std::byte> out;
    PngErrors errors;
    PngWriteGuard g;
    g.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &errors, png_on_error, png_on_warning);
    if (g.png == nullptr) {
        return {};
    }
    g.info = png_create_info_struct(g.png);
    if (g.info == nullptr) {
        return {};
    }
    const bool ok = guarded(g.png, [&] {
        png_set_write_fn(g.png, &out, png_write_bytes, png_flush_bytes);
        png_set_compression_level(g.png, 9);
        png_set_compression_mem_level(g.png, 9);
        if (pal) {
            png_set_IHDR(g.png,
                         g.info,
                         img.width,
                         img.height,
                         pal->bit_depth,
                         PNG_COLOR_TYPE_PALETTE,
                         PNG_INTERLACE_NONE,
                         PNG_COMPRESSION_TYPE_DEFAULT,
                         PNG_FILTER_TYPE_DEFAULT);
            png_set_PLTE(g.png, g.info, pal->colours.data(), static_cast<int>(pal->colours.size()));
            if (!pal->alpha.empty()) {
                png_set_tRNS(
                    g.png, g.info, pal->alpha.data(), static_cast<int>(pal->alpha.size()), nullptr);
            }
            png_set_filter(g.png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
        } else {
            png_set_IHDR(g.png,
                         g.info,
                         img.width,
                         img.height,
                         8,
                         colour_type,
                         PNG_INTERLACE_NONE,
                         PNG_COMPRESSION_TYPE_DEFAULT,
                         PNG_FILTER_TYPE_DEFAULT);
            png_set_filter(g.png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
        }
        if (!img.icc_profile.empty()) {
            png_set_iCCP(g.png,
                         g.info,
                         "ICC Profile",
                         PNG_COMPRESSION_TYPE_BASE,
                         reinterpret_cast<png_const_bytep>(img.icc_profile.data()),
                         static_cast<png_uint_32>(img.icc_profile.size()));
        } else if (img.png_srgb_intent) {
            png_set_sRGB(g.png, g.info, *img.png_srgb_intent);
        }
        if (img.png_gamma) {
            png_set_gAMA_fixed(g.png, g.info, *img.png_gamma);
        }
        if (img.png_chrm) {
            const auto& c = *img.png_chrm;
            png_set_cHRM_fixed(g.png, g.info, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        }
        png_write_info(g.png, g.info);
        if (pal && pal->bit_depth < 8) {
            png_set_packing(g.png);
        }
        png_write_image(g.png, rows.data());
        png_write_end(g.png, nullptr);
    });
    if (!ok || errors.warnings > 0) {
        return {};
    }
    return out;
}

}  // namespace

DecodeResult decode_jpeg(Bytes bytes) noexcept {
    try {
        return decode_jpeg_impl(bytes);
    } catch (const std::exception&) {
        return {std::nullopt, "out of memory"};
    }
}

DecodeResult decode_png(Bytes bytes) noexcept {
    try {
        return decode_png_impl(bytes);
    } catch (const std::exception&) {
        return {std::nullopt, "out of memory"};
    }
}

std::vector<std::byte> encode_jpeg(const Image& image, const JpegEncodeOptions& opts) noexcept {
    try {
        return encode_jpeg_impl(image, opts);
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<std::byte> encode_png(const Image& image) noexcept {
    try {
        return encode_png_impl(image);
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace pptctl
