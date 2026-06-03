// Golden tests for the extract subcommand. Each test runs extract_to() against
// a committed fixture and byte-compares the result (after path normalization
// for JSON) against a checked-in golden file in tests/golden/.
//
// To regenerate golden files after an intentional output change:
//   ./tests/golden/generate_golden.sh ./build/macos-release/pptctl

#include <doctest/doctest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

#ifndef PPTCTL_GOLDEN_DIR
#define PPTCTL_GOLDEN_DIR "tests/golden"
#endif

namespace pptctl::cmd {
int extract_to(const std::string& path, const std::string& format_str, std::ostream& os);
}

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

std::string read_golden(const char* name) {
    const std::string path = std::string(PPTCTL_GOLDEN_DIR) + "/" + name;
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.is_open(), "golden file missing: ", path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Replace the "file": "..." value with the canonical placeholder so that
// machine-specific absolute paths don't break the comparison.
std::string normalize_json_path(const std::string& json) {
    static const std::regex kFileField(R"("file": "[^"]*")");
    return std::regex_replace(json, kFileField, R"("file": "<path>")");
}

std::string run(const char* fixture_name, const char* format) {
    std::ostringstream oss;
    const int rc = pptctl::cmd::extract_to(fixture(fixture_name), format, oss);
    REQUIRE(rc == 0);
    return oss.str();
}

}  // namespace

// ── minimal ──────────────────────────────────────────────────────────────────

TEST_CASE("golden: minimal.json") {
    const auto got = normalize_json_path(run("minimal.pptx", "json"));
    const auto want = read_golden("minimal.json");
    CHECK(got == want);
}
TEST_CASE("golden: minimal.md") {
    const auto got = run("minimal.pptx", "md");
    const auto want = read_golden("minimal.md");
    CHECK(got == want);
}
TEST_CASE("golden: minimal.txt") {
    const auto got = run("minimal.pptx", "txt");
    const auto want = read_golden("minimal.txt");
    CHECK(got == want);
}

// ── three_slides ─────────────────────────────────────────────────────────────

TEST_CASE("golden: three_slides.json") {
    const auto got = normalize_json_path(run("three_slides.pptx", "json"));
    const auto want = read_golden("three_slides.json");
    CHECK(got == want);
}
TEST_CASE("golden: three_slides.md") {
    const auto got = run("three_slides.pptx", "md");
    const auto want = read_golden("three_slides.md");
    CHECK(got == want);
}
TEST_CASE("golden: three_slides.txt") {
    const auto got = run("three_slides.pptx", "txt");
    const auto want = read_golden("three_slides.txt");
    CHECK(got == want);
}

// ── images_deck ──────────────────────────────────────────────────────────────

TEST_CASE("golden: images_deck.json") {
    const auto got = normalize_json_path(run("images_deck.pptx", "json"));
    const auto want = read_golden("images_deck.json");
    CHECK(got == want);
}
TEST_CASE("golden: images_deck.md") {
    const auto got = run("images_deck.pptx", "md");
    const auto want = read_golden("images_deck.md");
    CHECK(got == want);
}
TEST_CASE("golden: images_deck.txt") {
    const auto got = run("images_deck.pptx", "txt");
    const auto want = read_golden("images_deck.txt");
    CHECK(got == want);
}

// ── notes_and_masters ────────────────────────────────────────────────────────

TEST_CASE("golden: notes_and_masters.json") {
    const auto got = normalize_json_path(run("notes_and_masters.pptx", "json"));
    const auto want = read_golden("notes_and_masters.json");
    CHECK(got == want);
}
TEST_CASE("golden: notes_and_masters.md") {
    const auto got = run("notes_and_masters.pptx", "md");
    const auto want = read_golden("notes_and_masters.md");
    CHECK(got == want);
}
TEST_CASE("golden: notes_and_masters.txt") {
    const auto got = run("notes_and_masters.pptx", "txt");
    const auto want = read_golden("notes_and_masters.txt");
    CHECK(got == want);
}

// ── tables_deck ──────────────────────────────────────────────────────────────

TEST_CASE("golden: tables_deck.json") {
    const auto got = normalize_json_path(run("tables_deck.pptx", "json"));
    const auto want = read_golden("tables_deck.json");
    CHECK(got == want);
}
TEST_CASE("golden: tables_deck.md") {
    const auto got = run("tables_deck.pptx", "md");
    const auto want = read_golden("tables_deck.md");
    CHECK(got == want);
}
TEST_CASE("golden: tables_deck.txt") {
    const auto got = run("tables_deck.pptx", "txt");
    const auto want = read_golden("tables_deck.txt");
    CHECK(got == want);
}

// ── charts_deck ──────────────────────────────────────────────────────────────

TEST_CASE("golden: charts_deck.json") {
    const auto got = normalize_json_path(run("charts_deck.pptx", "json"));
    const auto want = read_golden("charts_deck.json");
    CHECK(got == want);
}
TEST_CASE("golden: charts_deck.md") {
    const auto got = run("charts_deck.pptx", "md");
    const auto want = read_golden("charts_deck.md");
    CHECK(got == want);
}
TEST_CASE("golden: charts_deck.txt") {
    const auto got = run("charts_deck.pptx", "txt");
    const auto want = read_golden("charts_deck.txt");
    CHECK(got == want);
}
