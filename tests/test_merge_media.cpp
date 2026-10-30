// Tests for media de-duplication across merged decks (#137).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/merge_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

using pptctl::detail::MediaIndex;
using pptctl::detail::MergeTarget;

constexpr std::string_view kImageRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view kPackageRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/package";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_media_" + run + "_" + std::to_string(counter++));
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

std::vector<std::string> media_of(const pptctl::Package& pkg) {
    std::vector<std::string> out;
    for (const auto& name : pkg.part_names()) {
        if (name.starts_with("ppt/media/")) {
            out.push_back(name);
        }
    }
    return out;
}

struct MergeResult {
    pptctl::MergeReport report;
    pptctl::Package pkg;
};

void run_merge(const std::vector<std::string>& inputs, const fs::path& out, MergeResult& r) {
    std::string error;
    auto report = pptctl::merge(inputs, out.string(), {}, error);
    REQUIRE_MESSAGE(report, error);
    r.report = std::move(*report);
    REQUIRE(r.pkg.open(out.string()));
}

std::string messages(const std::vector<pptctl::Diagnostic>& ds) {
    std::string out;
    for (const auto& d : ds) {
        out += d.rule_id + ": " + d.message + "\n";
    }
    return out;
}

}  // namespace

// ── MediaIndex ────────────────────────────────────────────────────────────────

TEST_CASE("MediaIndex: matches identical bytes only, among same-size candidates") {
    pptctl::Package base;
    REQUIRE(base.open(fixture("images_deck.pptx")));  // 12 distinct solid-colour PNGs
    const auto media = media_of(base);
    REQUIRE(media.size() == 12);
    std::map<std::uint64_t, int> sizes;
    for (const auto& m : media) {
        ++sizes[*base.part_size(m)];
    }
    // Several images share a size, so the hash and byte checks do the work.
    REQUIRE(std::any_of(sizes.begin(), sizes.end(), [](const auto& e) { return e.second > 1; }));

    MergeTarget target(base);
    MediaIndex index(target);
    for (const auto& m : media) {
        CHECK(index.find(base, m, kImageRel) == m);
    }
}

TEST_CASE("MediaIndex: never matches outside ppt/media/ or through an exclusive relationship") {
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_a.pptx")));
    REQUIRE(source.open(fixture("merge_c.pptx")));
    MergeTarget target(base);
    MediaIndex index(target);
    CHECK(index.find(source, "ppt/media/image1.png", kImageRel) == "ppt/media/image1.png");
    CHECK_FALSE(index.find(source, "ppt/media/image1.png", kPackageRel));
    CHECK_FALSE(index.find(source, "ppt/theme/theme1.xml", ""));  // identical, but not media
    CHECK_FALSE(index.find(source, "ppt/media/missing.png", kImageRel));
}

TEST_CASE("MediaIndex: picks up media the target gains after it is built") {
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_b.pptx")));  // no media
    REQUIRE(source.open(fixture("merge_c.pptx")));
    MergeTarget target(base);
    MediaIndex index(target);
    CHECK_FALSE(index.find(source, "ppt/media/image1.png", kImageRel));
    target.add_part("ppt/media/image1.png", source.read_part("ppt/media/image1.png"), "image/png");
    CHECK(index.find(source, "ppt/media/image1.png", kImageRel) == "ppt/media/image1.png");
}

// ── merge ─────────────────────────────────────────────────────────────────────

TEST_CASE("merge: a logo both decks carry is stored once and shared") {
    ScratchDir dir;
    MergeResult m;
    run_merge({fixture("merge_a.pptx"), fixture("merge_b.pptx"), fixture("merge_c.pptx")},
              dir.path / "out.pptx",
              m);
    CHECK(media_of(m.pkg) == std::vector<std::string>{"ppt/media/image1.png"});
    const auto& c = m.report.inputs[2];
    CHECK(c.media_deduplicated == 1);
    pptctl::Package source;
    REQUIRE(source.open(fixture("merge_c.pptx")));
    CHECK(c.media_bytes_saved == *source.part_size("ppt/media/image1.png"));
    CHECK(m.report.inputs[1].media_deduplicated == 0);

    // Both of c's slides (now 6 and 7) point at a's logo.
    for (const auto* slide : {"ppt/slides/slide6.xml", "ppt/slides/slide7.xml"}) {
        bool found = false;
        for (const auto& rel : m.pkg.relationships(slide)) {
            found = found || rel.target == "ppt/media/image1.png";
        }
        CHECK_MESSAGE(found, slide);
    }
    const auto ds = pptctl::check_integrity(m.pkg, pptctl::PartGraph::build(m.pkg));
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

TEST_CASE("merge: media are matched by content, whatever their names") {
    // images_deck_v2 renames every media part, replaces one picture and adds one.
    ScratchDir dir;
    MergeResult m;
    run_merge(
        {fixture("images_deck.pptx"), fixture("images_deck_v2.pptx")}, dir.path / "out.pptx", m);
    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(fixture("images_deck.pptx")));
    REQUIRE(b.open(fixture("images_deck_v2.pptx")));
    std::set<std::uint64_t> in_a;
    for (const auto& name : media_of(a)) {
        in_a.insert(*a.part_hash(name));
    }
    std::size_t shared = 0;
    for (const auto& name : media_of(b)) {
        shared += in_a.contains(*b.part_hash(name)) ? 1 : 0;
    }
    REQUIRE(shared > 0);
    CHECK(m.report.inputs[1].media_deduplicated == shared);
    CHECK(media_of(m.pkg).size() == media_of(a).size() + media_of(b).size() - shared);
    const auto ds = pptctl::check_integrity(m.pkg, pptctl::PartGraph::build(m.pkg));
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

TEST_CASE("merge: a deck merged with itself adds no media") {
    ScratchDir dir;
    MergeResult m;
    run_merge({fixture("images_deck.pptx"), fixture("images_deck.pptx")}, dir.path / "out.pptx", m);
    CHECK(media_of(m.pkg).size() == 12);
    CHECK(m.report.inputs[1].media_deduplicated == 12);
    CHECK(m.report.slides == 24);
}

TEST_CASE("merge: the base's own duplicate media are left alone") {
    ScratchDir dir;
    // merge_a with a second, byte-identical copy of its logo.
    const auto base_path = dir.path / "dup_base.pptx";
    {
        pptctl::Package a;
        REQUIRE(a.open(fixture("merge_a.pptx")));
        pptctl::PartChanges changes;
        changes.replaced["ppt/media/image9.png"] = a.read_part("ppt/media/image1.png");
        REQUIRE(a.write(base_path.string(), changes));
    }
    MergeResult m;
    run_merge({base_path.string(), fixture("merge_c.pptx")}, dir.path / "out.pptx", m);
    CHECK(media_of(m.pkg) ==
          std::vector<std::string>{"ppt/media/image1.png", "ppt/media/image9.png"});
    CHECK(m.report.inputs[1].media_deduplicated == 1);
}
