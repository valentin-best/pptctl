// Tests for the "undeclared-ctype" integrity rule (#131): a part with no
// content type in [Content_Types].xml, or an Override for a missing part.

#include <doctest/doctest.h>

#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>
#include <string_view>

#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::run_rule;

namespace {

constexpr const char* kRule = "undeclared-ctype";
constexpr const char* kTypes = "[Content_Types].xml";

// minimal.pptx's [Content_Types].xml with `from` replaced by `to`.
std::string minimal_types_with(std::string_view from, std::string_view to) {
    pptctl::Package src;
    REQUIRE(src.open(integrity_test::fixture("minimal.pptx")));
    std::string xml = integrity_test::to_string(src.read_part(kTypes));
    const auto at = xml.find(from);
    REQUIRE(at != std::string::npos);
    xml.replace(at, from.size(), to);
    return xml;
}

}  // namespace

TEST_CASE("undeclared-ctype: the problems fixture has its two injected defects") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 2);
    CHECK(d[0].rule_id == "undeclared-ctype");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == "ppt/media/extra.webp");
    CHECK(d[0].message ==
          R"(part "ppt/media/extra.webp" has no content type: no Default for extension "webp" )"
          "and no Override");
    CHECK(d[1].severity == pptctl::Severity::Warning);
    CHECK(d[1].part == kTypes);
    CHECK(d[1].message ==
          R"(Override for "/ppt/slides/slide99.xml" names a part that does not exist)");
}

TEST_CASE("undeclared-ctype: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("undeclared-ctype: .rels parts need a declaration too") {
    pptctl::PartChanges changes;
    changes.replaced[kTypes] = integrity_test::to_bytes(minimal_types_with(
        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)",
        ""));
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE_FALSE(d.empty());
    for (const auto& diag : d) {
        CHECK(diag.part.ends_with(".rels"));
    }
}

TEST_CASE("undeclared-ctype: Default extensions and Override names match case-insensitively") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/a.PNG"] = integrity_test::to_bytes("png");
    changes.replaced["ppt/media/b.webp"] = integrity_test::to_bytes("webp");
    changes.replaced[kTypes] = integrity_test::to_bytes(
        minimal_types_with("<Default Extension=\"jpeg\"",
                           R"(<Default Extension="png" ContentType="image/png"/>)"
                           R"(<Override PartName="/PPT/Media/B.WEBP" ContentType="image/webp"/>)"
                           "<Default Extension=\"jpeg\""));
    const EditedDeck deck("minimal.pptx", changes);
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("undeclared-ctype: a part without an extension names that in its message") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/blob"] = integrity_test::to_bytes("x");
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message ==
          R"(part "ppt/media/blob" has no content type: no extension and no Override)");
}

TEST_CASE("undeclared-ctype: a missing [Content_Types].xml is one finding, not one per part") {
    pptctl::PartChanges changes;
    changes.removed.insert(kTypes);
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == kTypes);
    CHECK(d[0].message == "[Content_Types].xml is missing or not XML");
}
