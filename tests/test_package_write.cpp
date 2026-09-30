// Tests for Package::write and the content-types / rels rewrite helpers.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <pptctl/package.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path() / fs::path("pptctl_write_" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    static inline int counter = 0;
};

std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string to_string(const std::vector<std::byte>& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
    return out;
}

bool has(const std::vector<std::string>& names, std::string_view n) {
    return std::find(names.begin(), names.end(), n) != names.end();
}

}  // namespace

TEST_CASE("Package::write: empty changes round-trip every part byte-identically") {
    for (const char* name : {"three_slides.pptx", "images_deck.pptx"}) {
        CAPTURE(name);
        ScratchDir dir;
        pptctl::Package src;
        REQUIRE(src.open(fixture(name)));
        const auto out = (dir.path / "out.pptx").string();
        REQUIRE(src.write(out, pptctl::PartChanges{}));

        pptctl::Package dst;
        REQUIRE(dst.open(out));
        CHECK(dst.part_names() == src.part_names());
        for (const auto& part : src.part_names()) {
            CAPTURE(part);
            CHECK(dst.read_part(part) == src.read_part(part));
            CHECK(dst.part_size(part) == src.part_size(part));
            CHECK(dst.content_type(part) == src.content_type(part));
        }
    }
}

TEST_CASE("Package::write: replacing a part changes only that part and keeps its position") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("three_slides.pptx")));
    const auto out = (dir.path / "out.pptx").string();

    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide2.xml"] = to_bytes("<p:sld/>");
    REQUIRE(src.write(out, changes));

    pptctl::Package dst;
    REQUIRE(dst.open(out));
    CHECK(dst.part_names() == src.part_names());
    for (const auto& part : src.part_names()) {
        CAPTURE(part);
        if (part == "ppt/slides/slide2.xml") {
            CHECK(to_string(dst.read_part(part)) == "<p:sld/>");
        } else {
            CHECK(dst.read_part(part) == src.read_part(part));
        }
    }
}

TEST_CASE("Package::write: a replaced name that does not exist is appended as a new part") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("three_slides.pptx")));
    const auto out = (dir.path / "out.pptx").string();

    pptctl::PartChanges changes;
    changes.replaced["ppt/media/new.bin"] = to_bytes("hello");
    REQUIRE(src.write(out, changes));

    pptctl::Package dst;
    REQUIRE(dst.open(out));
    auto expected = src.part_names();
    expected.emplace_back("ppt/media/new.bin");
    CHECK(dst.part_names() == expected);
    CHECK(to_string(dst.read_part("ppt/media/new.bin")) == "hello");
}

TEST_CASE("Package::write: a removed part is absent from the output") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("three_slides.pptx")));
    const auto out = (dir.path / "out.pptx").string();
    REQUIRE(has(src.part_names(), "ppt/slides/slide3.xml"));

    pptctl::PartChanges changes;
    changes.removed.insert("ppt/slides/slide3.xml");
    REQUIRE(src.write(out, changes));

    pptctl::Package dst;
    REQUIRE(dst.open(out));
    CHECK_FALSE(has(dst.part_names(), "ppt/slides/slide3.xml"));
    CHECK(dst.part_count() == src.part_count() - 1);
    CHECK(dst.read_part("ppt/slides/slide1.xml") == src.read_part("ppt/slides/slide1.xml"));
}

TEST_CASE("Package::write: fails cleanly when the target directory does not exist") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("three_slides.pptx")));
    const auto out = (dir.path / "missing" / "out.pptx").string();
    CHECK_FALSE(src.write(out, pptctl::PartChanges{}));
    CHECK(fs::is_empty(dir.path));  // no temp file left behind
    CHECK(src.is_open());
}

TEST_CASE("Package::write: fails when no package is open") {
    ScratchDir dir;
    pptctl::Package pkg;
    CHECK_FALSE(pkg.write((dir.path / "out.pptx").string(), pptctl::PartChanges{}));
    CHECK(fs::is_empty(dir.path));
}

TEST_CASE("Package::write: in-place write replaces the file and the package reflects it") {
    ScratchDir dir;
    const auto path = (dir.path / "deck.pptx").string();
    fs::copy_file(fixture("three_slides.pptx"), path);

    pptctl::Package pkg;
    REQUIRE(pkg.open(path));
    const auto before = pkg.part_names();

    pptctl::PartChanges changes;
    changes.removed.insert("ppt/slides/slide3.xml");
    REQUIRE(pkg.write(path, changes));

    CHECK(pkg.is_open());
    CHECK_FALSE(has(pkg.part_names(), "ppt/slides/slide3.xml"));
    CHECK(pkg.part_count() == before.size() - 1);

    // Exactly one file in the directory: the temp file was renamed over the original.
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        (void)entry;
        ++files;
    }
    CHECK(files == 1);
}
