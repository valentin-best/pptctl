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

// ---------------------------------------------------------------------------
// Helpers: rels_part_for, rewrite_content_types, rewrite_rels
// ---------------------------------------------------------------------------

namespace {

std::size_t count_of(std::string_view haystack, std::string_view needle) {
    std::size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("rels_part_for maps a part to its _rels/*.rels sibling") {
    CHECK(pptctl::rels_part_for("ppt/slides/slide1.xml") == "ppt/slides/_rels/slide1.xml.rels");
    CHECK(pptctl::rels_part_for("ppt/presentation.xml") == "ppt/_rels/presentation.xml.rels");
    CHECK(pptctl::rels_part_for("") == "_rels/.rels");
}

TEST_CASE("rewrite_content_types drops the Override of a removed part and keeps the rest") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    const auto original = pkg.read_part("[Content_Types].xml");
    const auto before = to_string(original);
    REQUIRE(before.find("PartName=\"/ppt/slides/slide3.xml\"") != std::string::npos);

    const auto out =
        to_string(pptctl::rewrite_content_types(original, {"ppt/slides/slide3.xml"}, {}));
    CHECK(out.find("PartName=\"/ppt/slides/slide3.xml\"") == std::string::npos);
    CHECK(out.find("PartName=\"/ppt/slides/slide2.xml\"") != std::string::npos);
    CHECK(count_of(out, "<Override ") == count_of(before, "<Override ") - 1);
    CHECK(count_of(out, "<Default ") == count_of(before, "<Default "));
    CHECK(out.find("Extension=\"jpeg\"") != std::string::npos);
    CHECK(out.rfind("<?xml", 0) == 0);
    CHECK(out.find("standalone=\"yes\"") != std::string::npos);
    CHECK(out.find("xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"") !=
          std::string::npos);
}

TEST_CASE("rewrite_content_types adds an Override for a new part, replacing any existing one") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    const auto original = pkg.read_part("[Content_Types].xml");

    const auto out = to_string(
        pptctl::rewrite_content_types(original,
                                      {},
                                      {{"ppt/media/new.bin", "application/x-test"},
                                       {"ppt/slides/slide1.xml", "application/x-replaced"}}));
    CHECK(out.find(
              "<Override PartName=\"/ppt/media/new.bin\" ContentType=\"application/x-test\"/>") !=
          std::string::npos);
    CHECK(count_of(out, "PartName=\"/ppt/slides/slide1.xml\"") == 1);
    CHECK(out.find("PartName=\"/ppt/slides/slide1.xml\" ContentType=\"application/x-replaced\"") !=
          std::string::npos);
}

TEST_CASE("rewrite_content_types is a no-op for an empty change set") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    const auto original = pkg.read_part("[Content_Types].xml");
    const auto out = to_string(pptctl::rewrite_content_types(original, {}, {}));
    CHECK(count_of(out, "<Override ") == count_of(to_string(original), "<Override "));
    CHECK(count_of(out, "<Default ") == count_of(to_string(original), "<Default "));
}

TEST_CASE("rewrite_rels drops relationships whose target was removed and keeps other ids") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    const auto original = pkg.read_part("ppt/_rels/presentation.xml.rels");
    const auto before = to_string(original);
    REQUIRE(before.find("Target=\"slides/slide3.xml\"") != std::string::npos);

    const auto out = to_string(
        pptctl::rewrite_rels(original, "ppt/presentation.xml", {"ppt/slides/slide3.xml"}));
    CHECK(out.find("Target=\"slides/slide3.xml\"") == std::string::npos);
    CHECK(out.find("Target=\"slides/slide2.xml\"") != std::string::npos);
    CHECK(out.find("Target=\"slideMasters/slideMaster1.xml\"") != std::string::npos);
    CHECK(count_of(out, "<Relationship ") == count_of(before, "<Relationship ") - 1);
    // Surviving ids are untouched (no renumbering).
    CHECK(out.find("Id=\"rId1\"") != std::string::npos);
    CHECK(out.find("Id=\"rId8\"") != std::string::npos);
    CHECK(out.rfind("<?xml", 0) == 0);
    CHECK(out.find("xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"") !=
          std::string::npos);
}

TEST_CASE("rewrite_rels resolves ../ targets against the source part's directory") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto original = pkg.read_part("ppt/slides/_rels/slide1.xml.rels");
    REQUIRE(to_string(original).find("Target=\"../media/image1.png\"") != std::string::npos);

    const auto out = to_string(
        pptctl::rewrite_rels(original, "ppt/slides/slide1.xml", {"ppt/media/image1.png"}));
    CHECK(out.find("media/image1.png") == std::string::npos);
    CHECK(out.find("Target=\"../slideLayouts/slideLayout7.xml\"") != std::string::npos);
}

TEST_CASE("rewrite_rels never drops external relationships") {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"t\" Target=\"https://example.com/x\" "
        "TargetMode=\"External\"/>"
        "<Relationship Id=\"rId2\" Type=\"t\" Target=\"media/x\"/>"
        "</Relationships>";
    const auto out = to_string(pptctl::rewrite_rels(
        to_bytes(xml), "ppt/slides/slide1.xml", {"https://example.com/x", "ppt/slides/media/x"}));
    CHECK(out.find("Id=\"rId1\"") != std::string::npos);
    CHECK(out.find("Id=\"rId2\"") == std::string::npos);
}

TEST_CASE("Package::write end-to-end: removing a media part with its rels and content types") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    const std::string image = src.resolve("ppt/slides/slide1.xml", "rId2");
    REQUIRE(image == "ppt/media/image1.png");
    const std::string slide_rels = pptctl::rels_part_for("ppt/slides/slide1.xml");

    pptctl::PartChanges changes;
    changes.removed.insert(image);
    changes.replaced[slide_rels] =
        pptctl::rewrite_rels(src.read_part(slide_rels), "ppt/slides/slide1.xml", changes.removed);
    changes.replaced["[Content_Types].xml"] =
        pptctl::rewrite_content_types(src.read_part("[Content_Types].xml"), changes.removed, {});
    const auto out = (dir.path / "out.pptx").string();
    REQUIRE(src.write(out, changes));

    pptctl::Package dst;
    REQUIRE(dst.open(out));
    CHECK_FALSE(has(dst.part_names(), image));
    CHECK(dst.resolve("ppt/slides/slide1.xml", "rId2").empty());
    CHECK(dst.resolve("ppt/slides/slide1.xml", "rId1") == "ppt/slideLayouts/slideLayout7.xml");
    CHECK(dst.content_type("ppt/slides/slide1.xml") == src.content_type("ppt/slides/slide1.xml"));
    CHECK(dst.part_names().front() == "[Content_Types].xml");
}
