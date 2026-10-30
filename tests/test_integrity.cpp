// Tests for the package-integrity rule registry (#128). Each rule's own
// behaviour is tested in tests/test_integrity_<rule>.cpp.

#include <doctest/doctest.h>

#include <pptctl/integrity.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

}  // namespace

TEST_CASE("integrity: registry lists the six rules in run order") {
    std::vector<std::string> ids;
    for (const auto& rule : pptctl::integrity_rules()) {
        ids.push_back(rule.id);
    }
    CHECK(ids == std::vector<std::string>{"dangling-rel",
                                          "broken-rid",
                                          "undeclared-ctype",
                                          "orphan-part",
                                          "shared-part",
                                          "unlisted-part"});
}

TEST_CASE("integrity: a clean deck has no findings") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("lint_clean.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    CHECK(pptctl::check_integrity(pkg, graph).empty());
}

TEST_CASE("integrity: every finding carries its rule id and a part location") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("integrity_problems.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    for (const auto& d : pptctl::check_integrity(pkg, graph)) {
        CHECK_FALSE(d.rule_id.empty());
        CHECK_FALSE(d.part.empty());
    }
}
