// Tests for the merged deck's presentation-level parts (#136): slide order,
// masters, notes master, sections, slide size, app.xml counts.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/presentation.hpp>
#include <pugixml.hpp>
#include <random>
#include <set>
#include <string>
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
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_concat_" + run + "_" + std::to_string(counter++));
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

// The merged output of `inputs`, opened.
struct Merged {
    ScratchDir dir;
    std::string path = (dir.path / "out.pptx").string();
    pptctl::MergeReport report;
    pptctl::Package pkg;

    Merged(const std::vector<std::string>& inputs, pptctl::MergeOptions opts = {}) {
        std::string error;
        auto r = pptctl::merge(inputs, path, opts, error);
        REQUIRE_MESSAGE(r, error);
        report = std::move(*r);
        REQUIRE(pkg.open(path));
    }

    pugi::xml_document xml(const std::string& part) const {
        const auto bytes = pkg.read_part(part);
        pugi::xml_document doc;
        REQUIRE(doc.load_buffer(bytes.data(), bytes.size()));
        return doc;
    }

    // The title text of every slide, in slide-list order.
    std::vector<std::string> titles() const {
        pptctl::Presentation pres(pkg);
        REQUIRE(pres.load());
        std::vector<std::string> out;
        for (const auto& slide : pres.slide_parts()) {
            const auto doc = xml(slide);
            std::string text;
            for (const auto& t : doc.select_nodes("(//a:t)[1]")) {
                text = t.node().text().as_string();
            }
            out.push_back(text);
        }
        return out;
    }

    std::vector<pptctl::Diagnostic> integrity() const {
        return pptctl::check_integrity(pkg, pptctl::PartGraph::build(pkg));
    }
};

std::string messages(const std::vector<pptctl::Diagnostic>& ds) {
    std::string out;
    for (const auto& d : ds) {
        out += d.rule_id + ": " + d.message + "\n";
    }
    return out;
}

std::vector<std::string> abc() {
    return {fixture("merge_a.pptx"), fixture("merge_b.pptx"), fixture("merge_c.pptx")};
}

const std::vector<std::string> kAbcTitles{
    "Deck A", "A: bullets", "A: picture", "B: bullets", "B: chart", "C: bullets", "C: closing"};

}  // namespace

// ── slide list ────────────────────────────────────────────────────────────────

TEST_CASE("merge: slides in input order, and every integrity rule passes") {
    const Merged m(abc());
    CHECK(m.titles() == kAbcTitles);
    CHECK(m.report.slides == 7);
    CHECK(m.report.inputs[1].slides == 2);
    CHECK(m.report.inputs[2].slides == 2);
    for (const auto& input : m.report.inputs) {
        CHECK_MESSAGE(input.warnings.empty(), input.path);
    }
    const auto ds = m.integrity();
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

TEST_CASE("merge: with --theme=2 the base's slides still come second") {
    const Merged m(abc(), {.theme = 2});
    CHECK(m.titles() == kAbcTitles);
    // The base keeps its own slide ids; imported slides are numbered after them.
    const auto doc = m.xml("ppt/presentation.xml");
    std::vector<std::uint32_t> ids;
    for (const auto& n : doc.select_nodes("//p:sldIdLst/p:sldId")) {
        ids.push_back(n.node().attribute("id").as_uint());
    }
    CHECK(ids == std::vector<std::uint32_t>{258, 259, 260, 256, 257, 261, 262});
    const auto ds = m.integrity();
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

TEST_CASE("merge: many decks, including real PowerPoint output, pass every integrity rule") {
    const Merged m({fixture("three_slides.pptx"),
                    fixture("merge_b.pptx"),
                    fixture("notes_and_masters.pptx"),
                    fixture("charts_deck.pptx"),
                    fixture("images_deck.pptx"),
                    fixture("tables_deck.pptx")});
    CHECK(m.report.slides == 3 + 2 + 4 + 2 + 12 + 2);
    CHECK(m.titles().size() == m.report.slides);
    const auto ds = m.integrity();
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

// ── masters ───────────────────────────────────────────────────────────────────

TEST_CASE("merge: imported masters are registered; master and layout ids stay unique") {
    // Three decks with three different masters: merge_b's own theme, and
    // merge_c with one layout a byte longer than merge_a's.
    ScratchDir dir;
    const auto tweaked = (dir.path / "tweaked_c.pptx").string();
    {
        pptctl::Package c;
        REQUIRE(c.open(fixture("merge_c.pptx")));
        auto layout = c.read_part("ppt/slideLayouts/slideLayout7.xml");
        layout.push_back(std::byte{' '});
        pptctl::PartChanges changes;
        changes.replaced["ppt/slideLayouts/slideLayout7.xml"] = std::move(layout);
        REQUIRE(c.write(tweaked, changes));
    }
    const Merged m({fixture("merge_a.pptx"), fixture("merge_b.pptx"), tweaked});
    const auto doc = m.xml("ppt/presentation.xml");
    const auto masters = doc.select_nodes("//p:sldMasterIdLst/p:sldMasterId");
    REQUIRE(masters.size() == 3);

    std::set<std::uint32_t> ids;
    std::size_t count = 0;
    for (const auto& n : masters) {
        const auto id = n.node().attribute("id").as_uint();
        CHECK(id >= 2'147'483'648U);
        ids.insert(id);
        ++count;
        const auto part =
            m.pkg.resolve("ppt/presentation.xml", n.node().attribute("r:id").as_string());
        REQUIRE_FALSE(part.empty());
        const auto master = m.xml(part);
        for (const auto& l : master.select_nodes("//p:sldLayoutIdLst/p:sldLayoutId")) {
            const auto lid = l.node().attribute("id").as_uint();
            CHECK(lid >= 2'147'483'648U);
            ids.insert(lid);
            ++count;
        }
    }
    CHECK(count == 3 + 3 * 11);
    CHECK(ids.size() == count);
}

// ── notes ─────────────────────────────────────────────────────────────────────

TEST_CASE("merge: a base without notes master gets the first imported one") {
    const Merged m(abc());  // merge_a has no notes; merge_b brings a notes master
    const auto doc = m.xml("ppt/presentation.xml");
    const auto rid = doc.select_node("//p:notesMasterIdLst/p:notesMasterId")
                         .node()
                         .attribute("r:id")
                         .as_string();
    const auto master = m.pkg.resolve("ppt/presentation.xml", rid);
    CHECK(master == "ppt/notesMasters/notesMaster1.xml");
    // Placed right after the slide masters, as the schema orders them.
    CHECK(std::string_view(
              doc.child("p:presentation").child("p:sldMasterIdLst").next_sibling().name()) ==
          "p:notesMasterIdLst");
    for (const auto* notes :
         {"ppt/notesSlides/notesSlide1.xml", "ppt/notesSlides/notesSlide2.xml"}) {
        bool found = false;
        for (const auto& rel : m.pkg.relationships(notes)) {
            found = found || rel.target == master;
        }
        CHECK(found);
    }
}

TEST_CASE("merge: imported notes slides share the base's notes master") {
    // merge_b has a notes master; notes_and_masters brings four notes slides.
    const Merged m({fixture("merge_b.pptx"), fixture("notes_and_masters.pptx")});
    const auto names = m.pkg.part_names();
    CHECK(std::count_if(names.begin(), names.end(), [](const std::string& n) {
              return n.starts_with("ppt/notesMasters/") && n.ends_with(".xml");
          }) == 1);
    CHECK(std::count_if(names.begin(), names.end(), [](const std::string& n) {
              return n.starts_with("ppt/notesSlides/") && n.ends_with(".xml");
          }) == 6);
    const auto ds = m.integrity();
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

TEST_CASE("merge: a base that relates its notes master without listing it gets it listed") {
    // python-pptx output relates the notes master from presentation.xml but
    // leaves out p:notesMasterIdLst, and Keynote then refuses the deck. The
    // fixtures list it (#152), so rebuild merge_b without the list.
    ScratchDir dir;
    const auto unlisted = (dir.path / "unlisted_b.pptx").string();
    {
        pptctl::Package b;
        REQUIRE(b.open(fixture("merge_b.pptx")));
        const auto bytes = b.read_part("ppt/presentation.xml");
        std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::string list =
            R"(<p:notesMasterIdLst><p:notesMasterId r:id="rId8"/></p:notesMasterIdLst>)";
        const auto at = xml.find(list);
        REQUIRE(at != std::string::npos);
        xml.erase(at, list.size());
        const auto* p = reinterpret_cast<const std::byte*>(xml.data());
        pptctl::PartChanges changes;
        changes.replaced["ppt/presentation.xml"] = std::vector<std::byte>(p, p + xml.size());
        REQUIRE(b.write(unlisted, changes));
    }

    const Merged m({unlisted, fixture("merge_a.pptx")});
    const auto doc = m.xml("ppt/presentation.xml");
    const auto rid = doc.select_node("//p:notesMasterIdLst/p:notesMasterId")
                         .node()
                         .attribute("r:id")
                         .as_string();
    CHECK(std::string_view(rid) == "rId8");  // the base's own relationship, reused
    CHECK(m.pkg.resolve("ppt/presentation.xml", rid) == "ppt/notesMasters/notesMaster1.xml");
    const auto ds = m.integrity();
    CHECK_MESSAGE(ds.empty(), messages(ds));
}

// ── sections ──────────────────────────────────────────────────────────────────

namespace {

struct SectionView {
    std::string name;
    std::string guid;
    std::vector<std::uint32_t> ids;
};

std::vector<SectionView> sections_of(const Merged& m) {
    const auto doc = m.xml("ppt/presentation.xml");
    std::vector<SectionView> out;
    for (const auto& s : doc.select_nodes("//p14:sectionLst/p14:section")) {
        SectionView v{
            s.node().attribute("name").as_string(), s.node().attribute("id").as_string(), {}};
        for (const auto& id : s.node().select_nodes("p14:sldIdLst/p14:sldId")) {
            v.ids.push_back(id.node().attribute("id").as_uint());
        }
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace

TEST_CASE("merge: sections carry over; a deck without sections gets one named after it") {
    const Merged m(abc());
    const auto sections = sections_of(m);
    REQUIRE(sections.size() == 4);
    CHECK(sections[0].name == "merge_a");
    CHECK(sections[0].ids == std::vector<std::uint32_t>{256, 257, 258});
    CHECK(sections[1].name == "Intro");
    CHECK(sections[1].guid == "{6F1A3C52-0B7E-4D8A-9C21-5E4B7A2D9F10}");
    CHECK(sections[1].ids == std::vector<std::uint32_t>{259});
    CHECK(sections[2].name == "Data");
    CHECK(sections[2].ids == std::vector<std::uint32_t>{260});
    CHECK(sections[3].name == "merge_c");
    CHECK(sections[3].ids == std::vector<std::uint32_t>{261, 262});
    for (const auto& s : sections) {
        CHECK(s.guid.size() == 38);
        CHECK(s.guid.front() == '{');
    }
}

TEST_CASE("merge: a deck merged with itself gets fresh section GUIDs for the copy") {
    const Merged m({fixture("merge_b.pptx"), fixture("merge_b.pptx")});
    const auto sections = sections_of(m);
    REQUIRE(sections.size() == 4);
    std::set<std::string> guids;
    for (const auto& s : sections) {
        guids.insert(s.guid);
    }
    CHECK(guids.size() == 4);
    CHECK(sections[2].name == "Intro");
    CHECK(sections[2].ids == std::vector<std::uint32_t>{258});
}

TEST_CASE("merge: no sections anywhere, none in the output") {
    const Merged m({fixture("merge_a.pptx"), fixture("merge_c.pptx")});
    CHECK(sections_of(m).empty());
}

// ── slide size, app.xml ───────────────────────────────────────────────────────

TEST_CASE("merge: a deck with another slide size is merged unscaled, with a warning") {
    const Merged m({fixture("merge_a.pptx"), fixture("three_slides.pptx")});
    pptctl::Package other;
    REQUIRE(other.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(other);
    REQUIRE(pres.load());
    REQUIRE(pres.canvas().width_emu != 9'144'000);
    REQUIRE(m.report.inputs[1].warnings.size() == 1);
    CHECK(m.report.inputs[1].warnings[0].starts_with("slide size "));
    CHECK(m.report.inputs[1].warnings[0].find("base deck's 10.00 x 7.50 in") != std::string::npos);
    CHECK(m.report.inputs[0].warnings.empty());
}

TEST_CASE("merge: docProps/app.xml counts the merged slides and notes") {
    const Merged m(abc());
    const auto doc = m.xml("docProps/app.xml");
    CHECK(doc.child("Properties").child("Slides").text().as_int() == 7);
    CHECK(doc.child("Properties").child("Notes").text().as_int() == 2);
}
