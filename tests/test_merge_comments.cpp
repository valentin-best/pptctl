// Tests for carrying comment authors over in a merge (#159).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pugixml.hpp>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kLegacyCommentsType =
    "application/vnd.openxmlformats-officedocument.presentationml.comments+xml";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name + ".pptx";
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_comments_" + run + "_" + std::to_string(counter++));
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

pugi::xml_document xml(const pptctl::Package& pkg, const std::string& part) {
    const auto bytes = pkg.read_part(part);
    pugi::xml_document doc;
    REQUIRE_MESSAGE(doc.load_buffer(bytes.data(), bytes.size()), part);
    return doc;
}

// The part the presentation relates with a relationship type ending in `suffix`.
std::string presentation_target(const pptctl::Package& pkg, std::string_view suffix) {
    for (const auto& rel : pkg.relationships("ppt/presentation.xml")) {
        if (rel.type.ends_with(suffix)) {
            return rel.target;
        }
    }
    return {};
}

struct LegacyComment {
    std::uint32_t author = 0;
    std::uint32_t idx = 0;
    std::string author_name;  // resolved through the author list
    std::string text;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> parent;
};

// Every legacy comment in `pkg`, its author resolved by name.
std::vector<LegacyComment> legacy_comments(const pptctl::Package& pkg) {
    std::map<std::uint32_t, std::string> names;
    const auto authors_part = presentation_target(pkg, "/commentAuthors");
    if (!authors_part.empty()) {
        const auto doc = xml(pkg, authors_part);
        for (const auto& a : doc.child("p:cmAuthorLst").children("p:cmAuthor")) {
            names[a.attribute("id").as_uint()] = a.attribute("name").as_string();
        }
    }
    std::vector<LegacyComment> out;
    for (const auto& part : pkg.part_names()) {
        if (pkg.content_type(part) != kLegacyCommentsType) {
            continue;
        }
        const auto doc = xml(pkg, part);
        for (const auto& cm : doc.child("p:cmLst").children("p:cm")) {
            LegacyComment c;
            c.author = cm.attribute("authorId").as_uint();
            c.idx = cm.attribute("idx").as_uint();
            c.author_name = names.contains(c.author) ? names[c.author] : "<missing>";
            c.text = cm.child("p:text").text().as_string();
            if (const auto p = cm.select_node(".//*[local-name()='parentCm']").node()) {
                c.parent = {{p.attribute("authorId").as_uint(), p.attribute("idx").as_uint()}};
            }
            out.push_back(std::move(c));
        }
    }
    return out;
}

struct Merged {
    ScratchDir dir;
    pptctl::MergeReport report;
    pptctl::Package pkg;

    explicit Merged(const std::vector<const char*>& decks) {
        std::vector<std::string> inputs;
        for (const auto* d : decks) {
            inputs.push_back(fixture(d));
        }
        const auto out = (dir.path / "out.pptx").string();
        std::string error;
        auto r = pptctl::merge(inputs, out, {}, error);
        REQUIRE_MESSAGE(r, error);
        report = std::move(*r);
        REQUIRE(pkg.open(out));
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

// Each comment is still by the author its text names ("Ana: ...": Ana Lopez),
// no (author, idx) pair repeats, and each author's lastIdx covers its comments.
void check_legacy(const pptctl::Package& pkg, std::size_t expected) {
    const std::map<std::string, std::string> who{
        {"Ana", "Ana Lopez"}, {"Ben", "Ben Okafor"}, {"Chloe", "Chloe Martin"}};
    const auto comments = legacy_comments(pkg);
    CHECK(comments.size() == expected);
    std::set<std::pair<std::uint32_t, std::uint32_t>> keys;
    for (const auto& c : comments) {
        CAPTURE(c.text);
        CHECK(c.author_name == who.at(c.text.substr(0, c.text.find(':'))));
        CHECK(keys.insert({c.author, c.idx}).second);
    }
    for (const auto& c : comments) {
        if (c.parent) {
            CAPTURE(c.text);
            CHECK(keys.contains(*c.parent));
            const auto parent = std::find_if(comments.begin(), comments.end(), [&](const auto& p) {
                return p.author == c.parent->first && p.idx == c.parent->second;
            });
            REQUIRE(parent != comments.end());
            CHECK(parent->text == "Ana: source?");
        }
    }
    const auto doc = xml(pkg, presentation_target(pkg, "/commentAuthors"));
    for (const auto& a : doc.child("p:cmAuthorLst").children("p:cmAuthor")) {
        for (const auto& c : comments) {
            if (c.author == a.attribute("id").as_uint()) {
                CHECK(c.idx <= a.attribute("lastIdx").as_uint());
            }
        }
    }
}

}  // namespace

// ── legacy comments ───────────────────────────────────────────────────────────

TEST_CASE("merge comments: a base without authors gets the imported deck's") {
    const Merged m({"merge_a", "merge_comments_a"});
    const auto authors = presentation_target(m.pkg, "/commentAuthors");
    REQUIRE(authors == "ppt/commentAuthors.xml");
    CHECK(m.pkg.content_type(authors) ==
          "application/vnd.openxmlformats-officedocument.presentationml.commentAuthors+xml");
    check_legacy(m.pkg, 4);
    CHECK(m.report.inputs[1].warnings.empty());
    m.check_integrity();
}

TEST_CASE("merge comments: an author in both decks is one author; the others are added") {
    // merge_comments_b's Ben Okafor is author 0 there, author 1 in the base.
    const Merged m({"merge_comments_a", "merge_comments_b"});
    const auto doc = xml(m.pkg, "ppt/commentAuthors.xml");
    std::vector<std::string> names;
    for (const auto& a : doc.child("p:cmAuthorLst").children("p:cmAuthor")) {
        names.emplace_back(a.attribute("name").as_string());
    }
    CHECK(names == std::vector<std::string>{"Ana Lopez", "Ben Okafor", "Chloe Martin"});
    check_legacy(m.pkg, 6);
    m.check_integrity();
}

TEST_CASE("merge comments: a deck merged with itself renumbers the copies, replies included") {
    const Merged m({"merge_comments_a", "merge_comments_a"});
    const auto doc = xml(m.pkg, "ppt/commentAuthors.xml");
    CHECK(std::distance(doc.child("p:cmAuthorLst").children("p:cmAuthor").begin(),
                        doc.child("p:cmAuthorLst").children("p:cmAuthor").end()) == 2);
    check_legacy(m.pkg, 8);
    // The base's comments keep their numbers; the copies continue after them.
    for (const auto& a : doc.child("p:cmAuthorLst").children("p:cmAuthor")) {
        CHECK(a.attribute("lastIdx").as_uint() == 4);
    }
    m.check_integrity();
}

TEST_CASE("merge comments: the base's own comments and authors are untouched") {
    pptctl::Package base;
    REQUIRE(base.open(fixture("merge_comments_a")));
    const Merged m({"merge_comments_a", "merge_comments_b"});
    CHECK(m.pkg.read_part("ppt/comments/comment1.xml") ==
          base.read_part("ppt/comments/comment1.xml"));
    CHECK(m.pkg.read_part("ppt/comments/comment2.xml") ==
          base.read_part("ppt/comments/comment2.xml"));
}

// ── modern comments ───────────────────────────────────────────────────────────

TEST_CASE("merge comments: modern authors are carried over and the comment follows its slide") {
    const Merged m({"merge_a", "merge_comments_modern"});
    const auto authors = presentation_target(m.pkg, "/authors");
    REQUIRE(authors == "ppt/authors.xml");
    CHECK(m.pkg.content_type(authors) == "application/vnd.ms-powerpoint.authors+xml");
    const auto doc = xml(m.pkg, authors);
    const auto author = doc.child("p188:authorLst").child("p188:author");
    CHECK(std::string_view(author.attribute("name").as_string()) == "Dana Ito");

    // The comment names that author and anchors to the slide's new id (259:
    // merge_a's slides are 256-258).
    std::string comment_part;
    for (const auto& part : m.pkg.part_names()) {
        if (m.pkg.content_type(part) == "application/vnd.ms-powerpoint.comments+xml") {
            comment_part = part;
        }
    }
    REQUIRE_FALSE(comment_part.empty());
    const auto comment = xml(m.pkg, comment_part);
    const auto cm = comment.child("p188:cmLst").child("p188:cm");
    CHECK(std::string_view(cm.attribute("authorId").as_string()) ==
          author.attribute("id").as_string());
    CHECK(cm.select_node(".//*[local-name()='sldMk']").node().attribute("sldId").as_uint() == 259);
    m.check_integrity();
}

TEST_CASE("merge comments: a modern author already present isn't added twice") {
    const Merged m({"merge_comments_modern", "merge_comments_modern"});
    const auto doc = xml(m.pkg, "ppt/authors.xml");
    CHECK(std::distance(doc.child("p188:authorLst").children("p188:author").begin(),
                        doc.child("p188:authorLst").children("p188:author").end()) == 1);
    // The original comment keeps the base slide's id; the copy anchors to the
    // second slide's.
    std::multiset<std::uint32_t> anchors;
    for (const auto& part : m.pkg.part_names()) {
        if (m.pkg.content_type(part) == "application/vnd.ms-powerpoint.comments+xml") {
            anchors.insert(xml(m.pkg, part)
                               .select_node("//*[local-name()='sldMk']")
                               .node()
                               .attribute("sldId")
                               .as_uint());
        }
    }
    CHECK(anchors == std::multiset<std::uint32_t>{256, 257});
    m.check_integrity();
}
