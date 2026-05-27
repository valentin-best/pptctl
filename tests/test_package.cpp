// Tests for the Package and Presentation layers.

#include <doctest/doctest.h>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>

#include <algorithm>
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
