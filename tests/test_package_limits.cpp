// Tests for the decompression limits Package::open() enforces on hostile
// archives (see package.hpp).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name);
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_limits_" + run + "_" + std::to_string(counter++));
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

// minimal.pptx plus a part of `size` zero bytes (deflate packs zeros about
// 1000:1), written to `dir`.
std::string with_zeros(const ScratchDir& dir, std::size_t size) {
    pptctl::Package src;
    REQUIRE(src.open(fixture("minimal.pptx")));
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/zeros.bin"] = std::vector<std::byte>(size, std::byte{0});
    const auto out = (dir.path / ("zeros_" + std::to_string(size) + ".pptx")).string();
    REQUIRE(src.write(out, changes));
    return out;
}

void put16(std::string& out, std::uint32_t v) {
    out += static_cast<char>(v & 0xFFU);
    out += static_cast<char>((v >> 8U) & 0xFFU);
}

void put32(std::string& out, std::uint32_t v) {
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16U);
}

// A ZIP holding only a central directory whose stored (uncompressed) entries
// each declare `size` bytes: enough for open() to read the declared sizes,
// without the data a real archive of that size would need.
std::string declared_only_zip(const ScratchDir& dir, int entries, std::uint32_t size) {
    std::string cd;
    for (int i = 0; i < entries; ++i) {
        const std::string name = "ppt/media/big" + std::to_string(i) + ".bin";
        put32(cd, 0x02014b50U);  // central directory file header
        put16(cd, 20);           // version made by
        put16(cd, 20);           // version needed
        put16(cd, 0);            // flags
        put16(cd, 0);            // method: stored
        put16(cd, 0);            // time
        put16(cd, 0);            // date
        put32(cd, 0);            // crc-32
        put32(cd, size);         // compressed size
        put32(cd, size);         // uncompressed size
        put16(cd, static_cast<std::uint32_t>(name.size()));
        put16(cd, 0);  // extra length
        put16(cd, 0);  // comment length
        put16(cd, 0);  // disk number
        put16(cd, 0);  // internal attributes
        put32(cd, 0);  // external attributes
        put32(cd, 0);  // local header offset
        cd += name;
    }
    std::string zip = cd;
    put32(zip, 0x06054b50U);  // end of central directory
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<std::uint32_t>(entries));
    put16(zip, static_cast<std::uint32_t>(entries));
    put32(zip, static_cast<std::uint32_t>(cd.size()));
    put32(zip, 0);  // central directory offset
    put16(zip, 0);
    const auto out = (dir.path / "declared.pptx").string();
    std::ofstream(out, std::ios::binary) << zip;
    return out;
}

}  // namespace

TEST_CASE("limits: a part that would inflate far beyond any real deck is refused") {
    const ScratchDir dir;
    const auto bomb = with_zeros(dir, std::size_t{4} << 20);  // 4 MiB of zeros
    pptctl::Package pkg;
    CHECK_FALSE(pkg.open(bomb));
    CHECK_FALSE(pkg.is_open());
    CHECK(pkg.part_count() == 0);
    CHECK(pkg.error().starts_with(R"(part "ppt/media/zeros.bin" would inflate from )"));
    CHECK(pkg.error().find("to 4.0 MiB (over 200:1)") != std::string::npos);
    CHECK(pkg.error().ends_with("refusing it as a likely decompression bomb"));

    // The same Package opens a good deck afterwards, with no stale error.
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.error().empty());
}

TEST_CASE("limits: small parts aren't held to the ratio") {
    const ScratchDir dir;
    pptctl::Package pkg;
    CHECK(pkg.open(with_zeros(dir, (std::size_t{64} << 10) - 1)));
    CHECK(pkg.error().empty());
}

TEST_CASE("limits: parts that together declare more than 4 GiB are refused") {
    const ScratchDir dir;
    pptctl::Package pkg;
    // Two stored 1.5 GiB entries pass, at 1:1 and 3 GiB in all; a third doesn't.
    CHECK(pkg.open(declared_only_zip(dir, 2, std::uint32_t{3} << 29)));
    CHECK_FALSE(pkg.open(declared_only_zip(dir, 3, std::uint32_t{3} << 29)));
    CHECK(pkg.error() == "its parts would inflate to more than 4096.0 MiB in all; refusing it");
}

TEST_CASE("limits: an unreadable file sets no limit error") {
    pptctl::Package pkg;
    CHECK_FALSE(pkg.open(fixture("does-not-exist.pptx")));
    CHECK(pkg.error().empty());
}

TEST_CASE("limits: commands name the reason") {
    const ScratchDir dir;
    const auto bomb = with_zeros(dir, std::size_t{4} << 20);
    const std::vector<std::string> inputs{fixture("minimal.pptx"), bomb};
    std::string error;
    CHECK_FALSE(pptctl::merge(inputs, "", {}, error));
    CHECK(error.starts_with("cannot open '" + bomb + R"(': part "ppt/media/zeros.bin")"));
}

TEST_CASE("limits: every fixture opens") {
    for (const auto& entry : fs::directory_iterator(PPTCTL_FIXTURES_DIR)) {
        if (entry.path().extension() == ".pptx") {
            CAPTURE(entry.path().string());
            pptctl::Package pkg;
            CHECK(pkg.open(entry.path().string()));
        }
    }
}
