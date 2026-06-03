// Tests for the lint subcommand.

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace pptctl::cmd {
int lint_to(const std::string& path, std::ostream& os);
}

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

int run(const char* fixture_name, std::string& out) {
    std::ostringstream oss;
    const int rc = pptctl::cmd::lint_to(fixture(fixture_name), oss);
    out = oss.str();
    return rc;
}

}  // namespace

TEST_CASE("lint: minimal.pptx — no findings") {
    std::string out;
    CHECK(run("minimal.pptx", out) == 0);
    CHECK(out.empty());
}

TEST_CASE("lint: three_slides.pptx — no overflow") {
    std::string out;
    CHECK(run("three_slides.pptx", out) == 0);
    CHECK(out.find("overflow") == std::string::npos);
}
