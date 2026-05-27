// Tests for the Package and Presentation layers.

#include <doctest/doctest.h>

#include <algorithm>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <string>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {
std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}
}  // namespace

TEST_CASE("Package: default-constructed package is not open") {
    pptctl::Package pkg;
    CHECK_FALSE(pkg.is_open());
    CHECK(pkg.part_count() == 0);
}

TEST_CASE("Package: open fails on a nonexistent file") {
    pptctl::Package pkg;
    CHECK_FALSE(pkg.open("/nonexistent/path/to/nothing.pptx"));
}

TEST_CASE("Package: open succeeds on minimal.pptx and enumerates parts") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.is_open());
    CHECK(pkg.part_count() > 0);

    const auto names = pkg.part_names();
    // Every .pptx must contain these.
    auto has = [&](const std::string& n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    CHECK(has("[Content_Types].xml"));
    CHECK(has("ppt/presentation.xml"));
}

TEST_CASE("Package: read_part returns content for an existing part") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    const auto bytes = pkg.read_part("ppt/presentation.xml");
    CHECK(bytes.size() > 0);
    // Should look like XML
    CHECK(static_cast<char>(bytes[0]) == '<');
}

TEST_CASE("Package: read_part returns empty for missing part") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.read_part("does/not/exist.xml").empty());
}

TEST_CASE("Package: content_type resolves an explicit Override by part path") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.content_type("ppt/presentation.xml") ==
          "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
    CHECK(pkg.content_type("ppt/slides/slide1.xml") ==
          "application/vnd.openxmlformats-officedocument.presentationml.slide+xml");
}

TEST_CASE("Package: content_type accepts a leading-slash part path") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.content_type("/ppt/presentation.xml") ==
          "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
}

TEST_CASE("Package: content_type falls back to a Default by extension") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    // No Override for .rels parts; resolved via Default Extension="rels".
    CHECK(pkg.content_type("_rels/.rels") ==
          "application/vnd.openxmlformats-package.relationships+xml");
    // A .xml part with no Override falls back to Default Extension="xml".
    CHECK(pkg.content_type("ppt/_rels/presentation.xml.rels") ==
          "application/vnd.openxmlformats-package.relationships+xml");
}

TEST_CASE("Package: content_type is empty for an unknown extension") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.content_type("ppt/media/nope.zzz").empty());
}

TEST_CASE("Package: relationships parses the package root (_rels/.rels)") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    const auto rels = pkg.relationships("");
    CHECK(rels.size() == 4);

    auto by_id = [&](const std::string& id) -> const pptctl::Relationship* {
        for (const auto& r : rels) {
            if (r.id == id)
                return &r;
        }
        return nullptr;
    };
    const auto* r1 = by_id("rId1");
    REQUIRE(r1 != nullptr);
    CHECK(r1->target == "ppt/presentation.xml");
    CHECK_FALSE(r1->external);
    CHECK(r1->type.find("officeDocument") != std::string::npos);
}

TEST_CASE("Package: relationships resolves relative targets against the source dir") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    // slide1.xml.rels has Target="../slideLayouts/slideLayout7.xml".
    const auto rels = pkg.relationships("ppt/slides/slide1.xml");
    REQUIRE(rels.size() == 1);
    CHECK(rels[0].id == "rId1");
    CHECK(rels[0].target == "ppt/slideLayouts/slideLayout7.xml");
}

TEST_CASE("Package: relationships is empty for a part with no .rels file") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.relationships("ppt/theme/theme1.xml").empty());
}

TEST_CASE("Package: resolve returns the target part name for a known id") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.resolve("ppt/slides/slide1.xml", "rId1") == "ppt/slideLayouts/slideLayout7.xml");
    CHECK(pkg.resolve("", "rId1") == "ppt/presentation.xml");
}

TEST_CASE("Package: resolve is empty for an unknown id") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    CHECK(pkg.resolve("ppt/slides/slide1.xml", "rId999").empty());
}

TEST_CASE("Presentation: loads minimal.pptx — 1 slide, 16:9 canvas") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    CHECK(pres.slide_count() == 1);
    // 13.333 × 7.5 inches → ~12192000 × 6858000 EMU
    CHECK(pres.canvas().width_emu > 12000000);
    CHECK(pres.canvas().width_emu < 12300000);
    CHECK(pres.canvas().height_emu == 6858000);
    CHECK(pres.canvas().width_inches() == doctest::Approx(13.333).epsilon(0.01));
    CHECK(pres.canvas().height_inches() == doctest::Approx(7.5).epsilon(0.001));
    CHECK(pres.master_count() >= 1);
    CHECK(pres.layout_count() >= 1);
}

TEST_CASE("Presentation: loads three_slides.pptx — counts 3 slides") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    CHECK(pres.slide_count() == 3);
}

TEST_CASE("Presentation: parses the theme color scheme") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("minimal.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    const auto& scheme = pres.color_scheme();
    CHECK(scheme.name == "Office");
    // The standard Office scheme has all 12 slots populated.
    REQUIRE(scheme.colors.size() == 12);

    auto color_of = [&](const std::string& name) -> std::string {
        for (const auto& c : scheme.colors) {
            if (c.name == name) {
                return c.value;
            }
        }
        return {};
    };

    // Slots emitted in canonical OOXML order.
    CHECK(scheme.colors.front().name == "dk1");
    CHECK(scheme.colors.back().name == "folHlink");

    // dk1 comes from a:sysClr/@lastClr; accents from a:srgbClr/@val. Uppercased.
    CHECK(color_of("dk1") == "000000");
    CHECK(color_of("lt1") == "FFFFFF");
    CHECK(color_of("accent1") == "4F81BD");
    CHECK(color_of("hlink") == "0000FF");
    CHECK(color_of("folHlink") == "800080");
}
