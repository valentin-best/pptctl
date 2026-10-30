// Tests for collapsing identical slide masters across merged decks (#139).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/merge_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

using pptctl::detail::master_closure;
using pptctl::detail::MasterIndex;

constexpr const char* kMaster = "ppt/slideMasters/slideMaster1.xml";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_masters_" + run + "_" + std::to_string(counter++));
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

std::size_t count_parts(const pptctl::Package& pkg, std::string_view dir) {
    const auto names = pkg.part_names();
    return static_cast<std::size_t>(std::count_if(names.begin(), names.end(), [&](const auto& n) {
        return n.starts_with(dir) && n.ends_with(".xml");
    }));
}

struct Merged {
    ScratchDir dir;
    pptctl::MergeReport report;
    pptctl::Package pkg;

    explicit Merged(const std::vector<std::string>& inputs) {
        const auto out = (dir.path / "out.pptx").string();
        std::string error;
        auto r = pptctl::merge(inputs, out, {}, error);
        REQUIRE_MESSAGE(r, error);
        report = std::move(*r);
        REQUIRE(pkg.open(out));
    }

    [[nodiscard]] std::size_t masters() const {
        return count_parts(pkg, "ppt/slideMasters/");
    }
    [[nodiscard]] std::size_t layouts() const {
        return count_parts(pkg, "ppt/slideLayouts/");
    }
    void check_integrity() const {
        const auto ds = pptctl::check_integrity(pkg, pptctl::PartGraph::build(pkg));
        std::string messages;
        for (const auto& d : ds) {
            messages += d.rule_id + ": " + d.message + "\n";
        }
        CHECK_MESSAGE(ds.empty(), messages);
    }
};

// `in` with the bytes of `part` changed by one appended space.
std::string with_extra_space(const fs::path& dir, const char* in, const std::string& part) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture(in)));
    auto bytes = pkg.read_part(part);
    bytes.push_back(std::byte{' '});
    pptctl::PartChanges changes;
    changes.replaced[part] = std::move(bytes);
    const auto out = (dir / (std::string("tweaked_") + in)).string();
    REQUIRE(pkg.write(out, changes));
    return out;
}

}  // namespace

// ── master_closure ────────────────────────────────────────────────────────────

TEST_CASE("master_closure: the master, its layouts and its theme, master first") {
    pptctl::Package a;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    const auto closure = master_closure(a, kMaster);
    REQUIRE(closure.parts.size() == 1 + 11 + 1);
    CHECK(closure.parts.front() == kMaster);
    CHECK(std::count_if(closure.parts.begin(), closure.parts.end(), [](const auto& p) {
              return p.starts_with("ppt/slideLayouts/");
          }) == 11);
    CHECK(std::find(closure.parts.begin(), closure.parts.end(), "ppt/theme/theme1.xml") !=
          closure.parts.end());
}

TEST_CASE("master_closure: same template, same signature; another theme, another signature") {
    pptctl::Package a;
    pptctl::Package b;
    pptctl::Package c;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    REQUIRE(b.open(fixture("merge_b.pptx")));
    REQUIRE(c.open(fixture("merge_c.pptx")));
    CHECK(master_closure(a, kMaster).signature == master_closure(c, kMaster).signature);
    CHECK(master_closure(a, kMaster).signature != master_closure(b, kMaster).signature);
}

// ── MasterIndex ───────────────────────────────────────────────────────────────

TEST_CASE("MasterIndex: maps an identical master's whole closure onto the base's") {
    pptctl::Package a;
    pptctl::Package b;
    pptctl::Package c;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    REQUIRE(b.open(fixture("merge_b.pptx")));
    REQUIRE(c.open(fixture("merge_c.pptx")));
    const MasterIndex index(a);
    const auto map = index.match(c, kMaster);
    CHECK(map.size() == 13);
    for (const auto& [from, to] : map) {
        CHECK(from == to);  // same template, same names: each part onto its namesake
        CHECK(a.read_part(to) == c.read_part(from));
    }
    CHECK(index.match(b, kMaster).empty());
}

TEST_CASE("MasterIndex: matches an imported master under its output names") {
    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    REQUIRE(b.open(fixture("merge_b.pptx")));
    MasterIndex index(a);
    // Pretend b's master closure was imported under shifted names.
    std::map<std::string, std::string> mapping;
    for (const auto& part : master_closure(b, kMaster).parts) {
        mapping[part] = "out/" + part;
    }
    index.add(b, kMaster, mapping);
    const auto map = index.match(b, kMaster);
    CHECK(map.size() == mapping.size());
    CHECK(map == mapping);
}

// ── merge ─────────────────────────────────────────────────────────────────────

TEST_CASE("merge: decks from the same template share one master") {
    const Merged m({fixture("merge_a.pptx"), fixture("merge_c.pptx")});
    CHECK(m.masters() == 1);
    CHECK(m.layouts() == 11);
    CHECK(count_parts(m.pkg, "ppt/theme/") == 1);
    const auto& c = m.report.inputs[1];
    CHECK(c.slides == 2);
    CHECK(c.masters_reused == 1);
    CHECK(c.masters_imported == 0);
    // c's slides (3 and 4 of a are 1-3, so 4 and 5) sit on a's layouts.
    for (const auto* slide : {"ppt/slides/slide4.xml", "ppt/slides/slide5.xml"}) {
        for (const auto& rel : m.pkg.relationships(slide)) {
            if (rel.target.starts_with("ppt/slideLayouts/")) {
                pptctl::Package a;
                REQUIRE(a.open(fixture("merge_a.pptx")));
                CHECK_MESSAGE(a.part_size(rel.target), rel.target);
            }
        }
    }
    m.check_integrity();
}

TEST_CASE("merge: decks with different themes keep a master each") {
    const Merged m({fixture("merge_a.pptx"), fixture("merge_b.pptx")});
    CHECK(m.masters() == 2);
    CHECK(m.report.inputs[1].masters_imported == 1);
    CHECK(m.report.inputs[1].masters_reused == 0);
    m.check_integrity();
}

TEST_CASE("merge: a master imported from one deck is reused by a later one") {
    const Merged m({fixture("merge_a.pptx"),
                    fixture("merge_b.pptx"),
                    fixture("merge_c.pptx"),
                    fixture("merge_b.pptx")});
    CHECK(m.masters() == 2);
    CHECK(m.layouts() == 22);
    CHECK(m.report.inputs[1].masters_imported == 1);
    CHECK(m.report.inputs[2].masters_reused == 1);
    CHECK(m.report.inputs[3].masters_reused == 1);
    CHECK(m.report.inputs[3].masters_imported == 0);
    CHECK(m.report.slides == 3 + 2 + 2 + 2);
    m.check_integrity();
}

TEST_CASE("merge: a master that differs by one byte is imported, not reused") {
    ScratchDir dir;
    const auto tweaked =
        with_extra_space(dir.path, "merge_c.pptx", "ppt/slideLayouts/slideLayout7.xml");
    const Merged m({fixture("merge_a.pptx"), tweaked});
    CHECK(m.masters() == 2);
    CHECK(m.report.inputs[1].masters_imported == 1);
    CHECK(m.report.inputs[1].masters_reused == 0);
    m.check_integrity();
}
