// Tests for the diff engine: slide fingerprints, similarity, matching, the
// renderers, and the golden fixture pairs.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/slide.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef PPTCTL_GOLDEN_DIR
#define PPTCTL_GOLDEN_DIR "tests/golden"
#endif

namespace pptctl::cmd {
int diff_to(const std::string& a,
            const std::string& b,
            std::ostream& os,
            const DiffOptions& opts,
            const DiffOutputOptions& out);
}  // namespace pptctl::cmd

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A slide with one text shape whose paragraph is `text`. Shape id/name are
// fixed so two slides built from the same text are structurally identical.
std::string text_slide(const std::string& text, const char* name = "Title 1") {
    return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name=")") +
           name + R"("/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="100" y="200"/><a:ext cx="300" cy="400"/></a:xfrm></p:spPr>
      <p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>)" +
           text + R"(</a:t></a:r></a:p></p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
}

// Same as text_slide but with the shape nudged to a different offset.
std::string nudged_slide(const std::string& text) {
    std::string xml = text_slide(text);
    const auto pos = xml.find(R"(x="100")");
    xml.replace(pos, 7, R"(x="999")");
    return xml;
}

pptctl::SlideSummary summary_of(const std::string& xml, std::size_t index) {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    s.set_index(index);
    return pptctl::summarize(s);
}

std::vector<pptctl::SlideSummary> deck(const std::vector<std::string>& xmls) {
    std::vector<pptctl::SlideSummary> out;
    for (std::size_t i = 0; i < xmls.size(); ++i) {
        out.push_back(summary_of(xmls[i], i + 1));
    }
    return out;
}

}  // namespace

TEST_CASE("Diff: fingerprint — identical slides hash equal, edited text differs") {
    const auto a = summary_of(text_slide("Quarterly results"), 1);
    const auto b = summary_of(text_slide("Quarterly results"), 7);
    const auto c = summary_of(text_slide("Quarterly result"), 1);
    CHECK(a.fingerprint == b.fingerprint);  // index is not part of identity
    CHECK(a.fingerprint != c.fingerprint);
}

TEST_CASE("Diff: fingerprint — moving a shape changes the fingerprint") {
    const auto a = summary_of(text_slide("Agenda"), 1);
    const auto b = summary_of(nudged_slide("Agenda"), 1);
    CHECK(a.fingerprint != b.fingerprint);
}

TEST_CASE("Diff: similarity — same text is 1, unrelated text is 0, partial in between") {
    const auto a = summary_of(text_slide("alpha beta gamma delta"), 1);
    const auto same = summary_of(nudged_slide("alpha beta gamma delta"), 1);
    const auto partial = summary_of(text_slide("alpha beta epsilon zeta"), 1);
    const auto other = summary_of(text_slide("one two three four"), 1);
    CHECK(pptctl::slide_similarity(a, same) == doctest::Approx(1.0));
    CHECK(pptctl::slide_similarity(a, other) == doctest::Approx(0.0));
    const double p = pptctl::slide_similarity(a, partial);
    CHECK(p > 0.3);
    CHECK(p < 0.7);
}

TEST_CASE("Diff: match — identical decks yield all-unchanged, no moves") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    CHECK(report.identical());
    REQUIRE(report.slides.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(report.slides[i].status == pptctl::SlideStatus::Unchanged);
        CHECK(report.slides[i].a_index == i + 1);
        CHECK(report.slides[i].b_index == i + 1);
        CHECK_FALSE(report.slides[i].moved);
    }
}

TEST_CASE("Diff: match — reordered slides are matched by fingerprint and flagged moved") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("two"), text_slide("three"), text_slide("one")});
    const auto report = pptctl::match_slides(a, b, {});
    CHECK_FALSE(report.identical());
    REQUIRE(report.slides.size() == 3);
    std::size_t moved = 0;
    for (const auto& s : report.slides) {
        CHECK(s.status == pptctl::SlideStatus::Unchanged);
        if (s.moved) {
            ++moved;
            CHECK(s.a_index == 1);
            CHECK(s.b_index == 3);
        }
    }
    CHECK(moved == 1);  // "one" moved to the end; "two"/"three" kept their order
}

TEST_CASE("Diff: match — added slide is reported at its B position") {
    const auto a = deck({text_slide("one"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 3);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Added);
    CHECK(report.slides[1].a_index == 0);
    CHECK(report.slides[1].b_index == 2);
    // Inserting shifts B indices; that is not a move.
    CHECK(report.slides[2].status == pptctl::SlideStatus::Unchanged);
    CHECK_FALSE(report.slides[2].moved);
}

TEST_CASE("Diff: match — removed slide is reported at its A position") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 3);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Removed);
    CHECK(report.slides[1].a_index == 2);
    CHECK(report.slides[1].b_index == 0);
}

TEST_CASE("Diff: match — near-duplicate pairs as changed via best-match") {
    const auto a = deck({text_slide("one"), text_slide("revenue grew ten percent this quarter")});
    const auto b =
        deck({text_slide("one"), text_slide("revenue grew twelve percent this quarter")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Changed);
    CHECK(report.slides[1].a_index == 2);
    CHECK(report.slides[1].b_index == 2);
    CHECK(report.slides[1].similarity > 0.5);
}

TEST_CASE("Diff: match — below threshold is added + removed, not changed") {
    const auto a = deck({text_slide("alpha beta gamma")});
    const auto b = deck({text_slide("one two three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[0].status == pptctl::SlideStatus::Removed);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Added);
}

TEST_CASE("Diff: match — best-match prefers the most similar candidate") {
    // A2 has two plausible partners in B; the closer one must win.
    const auto a = deck({text_slide("alpha beta gamma delta")});
    const auto b = deck({text_slide("alpha beta zero zero"), text_slide("alpha beta gamma zero")});
    const auto report = pptctl::match_slides(a, b, {});
    bool paired_with_2 = false;
    for (const auto& s : report.slides) {
        if (s.status == pptctl::SlideStatus::Changed) {
            paired_with_2 = (s.b_index == 2);
        }
    }
    CHECK(paired_with_2);
}

TEST_CASE("Diff: match — empty decks are identical") {
    const auto report = pptctl::match_slides({}, {}, {});
    CHECK(report.identical());
    CHECK(report.slides.empty());
}

TEST_CASE("Diff: command — self-diff of a fixture is identical, exit 0") {
    std::ostringstream os;
    const int rc = pptctl::cmd::diff_to(
        fixture("three_slides.pptx"), fixture("three_slides.pptx"), os, {}, {});
    CHECK(rc == 0);
    CHECK(os.str().empty());
}

TEST_CASE("Diff: command — different fixtures exit 1 and describe slide ops") {
    std::ostringstream os;
    const int rc =
        pptctl::cmd::diff_to(fixture("minimal.pptx"), fixture("three_slides.pptx"), os, {}, {});
    CHECK(rc == 1);
    CHECK(os.str().find("added") != std::string::npos);
}

TEST_CASE("Diff: command — unreadable input exits 2") {
    std::ostringstream os;
    const int rc = pptctl::cmd::diff_to(fixture("nope.pptx"), fixture("minimal.pptx"), os, {}, {});
    CHECK(rc == 2);
}

// ── op seams (scaffold for #70 / #72 / #73) ───────────────────────────────────

namespace pptctl::cmd {
std::optional<DiffReport> diff_report(const std::string& a,
                                      const std::string& b,
                                      const DiffOptions& opts);
}  // namespace pptctl::cmd

TEST_CASE("Diff: compare_slides — identical slides yield no changes") {
    pptctl::Slide a;
    pptctl::Slide b;
    REQUIRE(a.parse(text_slide("same")));
    REQUIRE(b.parse(text_slide("same")));
    const auto changes = pptctl::compare_slides(a, b, {});
    CHECK(changes.empty());
}

TEST_CASE("Diff: diff_report — changed pairs carry a (possibly empty) change set") {
    const auto r =
        pptctl::cmd::diff_report(fixture("three_slides.pptx"), fixture("three_slides.pptx"), {});
    REQUIRE(r);
    CHECK(r->identical());
    for (const auto& s : r->slides) {
        CHECK(s.changes.empty());
    }
}

TEST_CASE("Diff: diff_report — unreadable input yields nullopt") {
    CHECK_FALSE(pptctl::cmd::diff_report(fixture("nope.pptx"), fixture("minimal.pptx"), {}));
}

// ── output formats (#71) ──────────────────────────────────────────────────────

namespace {

// Run the command with the given output settings and capture stdout.
int run_diff(const char* a, const char* b, std::string& out, pptctl::DiffOutputOptions opts = {}) {
    std::ostringstream os;
    const int rc = pptctl::cmd::diff_to(fixture(a), fixture(b), os, {}, opts);
    out = os.str();
    return rc;
}

bool has_escape_codes(const std::string& s) {
    return s.find('\x1b') != std::string::npos;
}

std::string read_golden(const char* name) {
    const std::string path = std::string(PPTCTL_GOLDEN_DIR) + "/" + name;
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.is_open(), "golden file missing: ", path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The three renderers all carry the (machine-specific) input paths exactly
// once each: the `---`/`+++` header lines, the `a`/`b` JSON fields, and the
// `<code class="path">` elements. Same rewrite as generate_golden.sh.
std::string normalize_paths(const std::string& s) {
    static const std::regex kUnified(R"re(^(---|\+\+\+) .*$)re", std::regex::multiline);
    static const std::regex kJson(R"re("(a|b)": "[^"]*")re");
    static const std::regex kHtml(R"re(<code class="path">[^<]*</code>)re");
    std::string out = std::regex_replace(s, kUnified, "$1 <path>");
    out = std::regex_replace(out, kJson, R"re("$1": "<path>")re");
    out = std::regex_replace(out, kHtml, R"re(<code class="path">&lt;path&gt;</code>)re");
    return out;
}

void check_diff_golden(const char* a, const char* b, const char* format, const char* golden) {
    std::string out;
    const int rc = run_diff(a, b, out, {.format = format});
    CHECK(rc <= 1);
    CHECK(normalize_paths(out) == read_golden(golden));
}

}  // namespace

TEST_CASE("Diff: unified — identical decks print nothing at all") {
    std::string out;
    CHECK(run_diff("three_slides.pptx", "three_slides.pptx", out, {.format = "unified"}) == 0);
    CHECK(out.empty());
}

TEST_CASE("Diff: unified — different decks print a file header, hunks and a summary line") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.format = "unified"}) == 1);
    CHECK(out.find("--- ") == 0);
    CHECK(out.find("\n+++ ") != std::string::npos);
    CHECK(out.find("@@ slide ") != std::string::npos);
    CHECK(out.find("(added) @@") != std::string::npos);
    CHECK(out.find(" slides: ") != std::string::npos);
    CHECK(out.find(" added, ") != std::string::npos);
    CHECK(out.find(" removed, ") != std::string::npos);
    CHECK(out.find(" moved, ") != std::string::npos);
    CHECK(out.find(" changed\n") != std::string::npos);
}

TEST_CASE("Diff: unified — colour is off by default on a non-TTY stream and with --no-color") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {}) == 1);
    CHECK_FALSE(has_escape_codes(out));
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.color = false}) == 1);
    CHECK_FALSE(has_escape_codes(out));
}

TEST_CASE("Diff: unified — --color forces ANSI escapes even on a non-TTY stream") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.color = true}) == 1);
    CHECK(has_escape_codes(out));
    CHECK(out.find("\x1b[36m@@ slide") != std::string::npos);  // hunk headers are cyan
    CHECK(out.find("\x1b[0m") != std::string::npos);           // and every colour is reset
    // Stripping the escapes yields exactly the plain rendering.
    static const std::regex kAnsi("\x1b\\[[0-9;]*m");
    std::string plain;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", plain, {.color = false}) == 1);
    CHECK(std::regex_replace(out, kAnsi, "") == plain);
}

TEST_CASE("Diff: json — parses, is versioned, and includes unchanged slides") {
    std::string out;
    CHECK(run_diff("three_slides.pptx", "three_slides.pptx", out, {.format = "json"}) == 0);
    const auto j = nlohmann::json::parse(out);
    CHECK(j["schema_version"] == 1);
    CHECK(j["a"].is_string());
    CHECK(j["b"].is_string());
    REQUIRE(j["slides"].size() == 3);
    for (const auto& s : j["slides"]) {
        CHECK(s["status"] == "unchanged");
        CHECK(s["a_index"] == s["b_index"]);
        CHECK(s["moved"] == false);
        CHECK(s["similarity"] == 1.0);
        CHECK(s["changes"]["text"].is_array());
        CHECK(s["changes"]["shapes"].is_array());
        CHECK(s["changes"]["pictures"].is_array());
    }
    CHECK(j["summary"]["unchanged"] == 3);
    CHECK(j["summary"]["added"] == 0);
}

TEST_CASE("Diff: json — added slides have a null a_index, removed slides a null b_index") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.format = "json"}) == 1);
    auto j = nlohmann::json::parse(out);
    bool saw_added = false;
    for (const auto& s : j["slides"]) {
        if (s["status"] == "added") {
            saw_added = true;
            CHECK(s["a_index"].is_null());
            CHECK(s["b_index"].is_number());
        }
    }
    CHECK(saw_added);

    CHECK(run_diff("three_slides.pptx", "minimal.pptx", out, {.format = "json"}) == 1);
    j = nlohmann::json::parse(out);
    bool saw_removed = false;
    for (const auto& s : j["slides"]) {
        if (s["status"] == "removed") {
            saw_removed = true;
            CHECK(s["a_index"].is_number());
            CHECK(s["b_index"].is_null());
        }
    }
    CHECK(saw_removed);
}

TEST_CASE("Diff: html — self-contained document with a status badge per slide, text escaped") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.format = "html"}) == 1);
    std::string lower = out;
    for (auto& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    CHECK(lower.find("<!doctype html>") == 0);
    CHECK(lower.find("<style>") != std::string::npos);
    CHECK(lower.find("<script") == std::string::npos);
    CHECK(lower.find("http://") == std::string::npos);  // no external assets
    CHECK(lower.find("https://") == std::string::npos);
    CHECK(out.find("badge added") != std::string::npos);
    // Every '<' opens a tag or a comment: nothing from the decks leaks unescaped.
    for (std::size_t i = out.find('<'); i != std::string::npos; i = out.find('<', i + 1)) {
        REQUIRE(i + 1 < out.size());
        const char next = out[i + 1];
        CHECK((std::isalpha(static_cast<unsigned char>(next)) || next == '/' || next == '!'));
    }
    // Identical decks still get a full page (unlike unified).
    CHECK(run_diff("three_slides.pptx", "three_slides.pptx", out, {.format = "html"}) == 0);
    CHECK(out.find("badge unchanged") != std::string::npos);
}

TEST_CASE("Diff: --quiet prints nothing but keeps the exit code") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.quiet = true}) == 1);
    CHECK(out.empty());
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.format = "json", .quiet = true}) ==
          1);
    CHECK(out.empty());
    CHECK(run_diff("three_slides.pptx", "three_slides.pptx", out, {.quiet = true}) == 0);
    CHECK(out.empty());
}

TEST_CASE("Diff: unknown --format exits 2 without writing to stdout") {
    std::string out;
    CHECK(run_diff("minimal.pptx", "three_slides.pptx", out, {.format = "yaml"}) == 2);
    CHECK(out.empty());
}

// ── golden tests ──────────────────────────────────────────────────────────────
//
// tests/golden/<a>__<b>.diff.{txt,json,html} pin each renderer's output for
// one differing pair and one self-diff. Regenerate after an intentional
// output change: ./tests/golden/generate_golden.sh ./build/macos-release/pptctl

TEST_CASE("golden: minimal__three_slides.diff.txt") {
    check_diff_golden(
        "minimal.pptx", "three_slides.pptx", "unified", "minimal__three_slides.diff.txt");
}
TEST_CASE("golden: minimal__three_slides.diff.json") {
    check_diff_golden(
        "minimal.pptx", "three_slides.pptx", "json", "minimal__three_slides.diff.json");
}
TEST_CASE("golden: minimal__three_slides.diff.html") {
    check_diff_golden(
        "minimal.pptx", "three_slides.pptx", "html", "minimal__three_slides.diff.html");
}
TEST_CASE("golden: three_slides__three_slides.diff.txt") {
    check_diff_golden(
        "three_slides.pptx", "three_slides.pptx", "unified", "three_slides__three_slides.diff.txt");
}
TEST_CASE("golden: three_slides__three_slides.diff.json") {
    check_diff_golden(
        "three_slides.pptx", "three_slides.pptx", "json", "three_slides__three_slides.diff.json");
}
TEST_CASE("golden: three_slides__three_slides.diff.html") {
    check_diff_golden(
        "three_slides.pptx", "three_slides.pptx", "html", "three_slides__three_slides.diff.html");
}

// ── nested-op rendering (renderer wiring after #70/#72/#73) ──────────────────

namespace pptctl::cmd {
void render(const DiffReport& report,
            const std::string& a,
            const std::string& b,
            std::ostream& os,
            const DiffOutputOptions& out);
}  // namespace pptctl::cmd

namespace {

// One changed slide carrying one op of every family, for renderer tests.
pptctl::DiffReport sample_report() {
    pptctl::DiffReport r;
    pptctl::SlideDiff s;
    s.status = pptctl::SlideStatus::Changed;
    s.a_index = 2;
    s.b_index = 2;
    s.similarity = 0.8;

    pptctl::TextChange changed;
    changed.kind = pptctl::TextChangeKind::Changed;
    changed.shape = "Title 1";
    changed.a_paragraph = 1;
    changed.b_paragraph = 1;
    changed.old_text = "Hello <old>";
    changed.new_text = "World";
    s.changes.text.push_back(changed);

    pptctl::TextChange inserted;
    inserted.kind = pptctl::TextChangeKind::Inserted;
    inserted.shape = "Content 2";
    inserted.b_paragraph = 3;
    inserted.new_text = "New bullet";
    s.changes.text.push_back(inserted);

    pptctl::TextChange deleted;
    deleted.kind = pptctl::TextChangeKind::Deleted;
    deleted.shape = "Content 2";
    deleted.a_paragraph = 2;
    deleted.old_text = "Gone";
    s.changes.text.push_back(deleted);

    pptctl::TextChange reformatted;
    reformatted.kind = pptctl::TextChangeKind::Reformatted;
    reformatted.shape = "Content 2";
    reformatted.a_paragraph = 1;
    reformatted.b_paragraph = 1;
    reformatted.old_text = "Same";
    reformatted.new_text = "Same";
    reformatted.formatting = "\"Same\": bold on";
    s.changes.text.push_back(reformatted);

    pptctl::ShapeChange removed;
    removed.op = pptctl::ShapeOp::Removed;
    removed.name = "Box 7";
    s.changes.shapes.push_back(removed);

    pptctl::PictureChange pic;
    pic.op = pptctl::PictureOp::Added;
    pic.shape_name = "Picture 9";
    s.changes.pictures.push_back(pic);

    r.slides.push_back(s);
    return r;
}

std::string render_as(const char* format) {
    std::ostringstream os;
    pptctl::DiffOutputOptions out;
    out.format = format;
    out.color = false;
    pptctl::cmd::render(sample_report(), "a.pptx", "b.pptx", os, out);
    return os.str();
}

}  // namespace

TEST_CASE("Diff: render — unified shows text ops as -/+ lines and other ops as summaries") {
    const std::string u = render_as("unified");
    CHECK(u.find("-Hello <old>\n") != std::string::npos);
    CHECK(u.find("+World\n") != std::string::npos);
    CHECK(u.find("+New bullet\n") != std::string::npos);
    CHECK(u.find("-Gone\n") != std::string::npos);
    CHECK(u.find("bold on") != std::string::npos);
    CHECK(u.find(pptctl::describe(sample_report().slides[0].changes.shapes[0])) !=
          std::string::npos);
    CHECK(u.find(pptctl::describe(sample_report().slides[0].changes.pictures[0])) !=
          std::string::npos);
    // Text lines come first, then structure, so a hunk reads like a text diff.
    CHECK(u.find("+World") < u.find("Box 7"));
}

TEST_CASE("Diff: render — json fills the three changes arrays via to_json") {
    const auto j = nlohmann::json::parse(render_as("json"));
    const auto& c = j["slides"][0]["changes"];
    REQUIRE(c["text"].size() == 4);
    CHECK(c["text"][0]["kind"] == "changed");
    CHECK(c["text"][0]["new_text"] == "World");
    REQUIRE(c["shapes"].size() == 1);
    CHECK(c["shapes"][0]["op"] == "removed");
    REQUIRE(c["pictures"].size() == 1);
    CHECK(c["pictures"][0]["op"] == "added");
}

TEST_CASE("Diff: render — html lists every op, escaped") {
    const std::string h = render_as("html");
    std::size_t items = 0;
    for (std::size_t pos = h.find("<li"); pos != std::string::npos; pos = h.find("<li", pos + 1)) {
        ++items;
    }
    CHECK(items == 6);
    CHECK(h.find("Hello &lt;old&gt;") != std::string::npos);
    CHECK(h.find("Hello <old>") == std::string::npos);
}

TEST_CASE("Diff: command — unified end to end shows picture ops for the image-swap fixture") {
    std::ostringstream os;
    pptctl::DiffOutputOptions out;
    out.color = false;
    const int rc = pptctl::cmd::diff_to(
        fixture("images_deck.pptx"), fixture("images_deck_v2.pptx"), os, {}, out);
    CHECK(rc == 1);
    CHECK(os.str().find("replaced") != std::string::npos);
    CHECK(os.str().find("alt text") != std::string::npos);
}

// ── golden fixture pairs (#75) ────────────────────────────────────────────────
//
// diff_base.pptx and its six one-edit variants (build_diff_deck in
// tests/fixtures/generate_fixtures.py) pin what every renderer says about each
// edit family, as tests/golden/diff_base__<variant>.diff.{txt,json,html}; the
// self-pair pins the empty report. Each golden must show exactly the edit the
// variant makes and nothing else, so a spurious op here is an engine finding.
// Regenerate after an intentional output change:
//   ./tests/golden/generate_golden.sh ./build/macos-release/pptctl

namespace {

struct GoldenFormat {
    const char* format;
    const char* ext;
};

constexpr GoldenFormat kGoldenFormats[] = {{"unified", "txt"}, {"json", "json"}, {"html", "html"}};

// diff_base.pptx against <variant>.pptx in every format.
void check_diff_pair_goldens(const char* variant) {
    const std::string b = std::string(variant) + ".pptx";
    for (const auto& [format, ext] : kGoldenFormats) {
        const std::string golden = std::format("diff_base__{}.diff.{}", variant, ext);
        CAPTURE(golden);
        check_diff_golden("diff_base.pptx", b.c_str(), format, golden.c_str());
    }
}

}  // namespace

TEST_CASE("golden: diff_base__diff_base.diff.*") {
    check_diff_pair_goldens("diff_base");
}
TEST_CASE("golden: diff_base__diff_text_edit.diff.*") {
    check_diff_pair_goldens("diff_text_edit");
}
TEST_CASE("golden: diff_base__diff_reorder.diff.*") {
    check_diff_pair_goldens("diff_reorder");
}
TEST_CASE("golden: diff_base__diff_slide_added.diff.*") {
    check_diff_pair_goldens("diff_slide_added");
}
TEST_CASE("golden: diff_base__diff_slide_removed.diff.*") {
    check_diff_pair_goldens("diff_slide_removed");
}
TEST_CASE("golden: diff_base__diff_shape_edit.diff.*") {
    check_diff_pair_goldens("diff_shape_edit");
}
TEST_CASE("golden: diff_base__diff_table_edit.diff.*") {
    check_diff_pair_goldens("diff_table_edit");
}

TEST_CASE("golden: the identical pair prints an empty unified report and exits 0") {
    std::string out;
    CHECK(run_diff("diff_base.pptx", "diff_base.pptx", out, {.format = "unified"}) == 0);
    CHECK(out.empty());
    CHECK(read_golden("diff_base__diff_base.diff.txt").empty());
}

// Cheap regression net: no fixture, however exotic, may differ from itself.
TEST_CASE("Diff: self-diff of every fixture in tests/fixtures is empty") {
    std::size_t decks = 0;
    for (const auto& entry : std::filesystem::directory_iterator(PPTCTL_FIXTURES_DIR)) {
        if (entry.path().extension() != ".pptx") {
            continue;
        }
        ++decks;
        const std::string path = entry.path().string();
        CAPTURE(path);
        std::ostringstream os;
        const int rc = pptctl::cmd::diff_to(path, path, os, {}, {.format = "unified"});
        CHECK(rc == 0);
        CHECK(os.str().empty());
    }
    CHECK(decks >= 17);  // 10 pre-existing fixtures + diff_base and its 6 variants
}
