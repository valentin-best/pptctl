// Tests for the picture-level diff (Phase 4). Owned by the picture diff issue; keep
// picture-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

constexpr const char* kHead = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>)";
constexpr const char* kTail = R"(
  </p:spTree></p:cSld>
</p:sld>)";

// A p:pic with the given cNvPr attributes and blip reference. `link` picks
// r:link over r:embed; `extra` lands inside p:cNvPr (e.g. descr="...").
std::string pic(
    const char* id, const char* name, const char* rid, bool link = false, const char* extra = "") {
    std::string out = "\n    <p:pic><p:nvPicPr><p:cNvPr id=\"";
    out += id;
    out += "\" name=\"";
    out += name;
    out += "\" ";
    out += extra;
    out += "/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip ";
    out += link ? "r:link=\"" : "r:embed=\"";
    out += rid;
    out += "\"/></p:blipFill><p:spPr/></p:pic>";
    return out;
}

std::string slide_xml(const std::string& body) {
    return std::string(kHead) + body + kTail;
}

pptctl::Slide parse(const std::string& xml) {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    return s;
}

std::vector<pptctl::PictureChange> pictures_between(const std::string& a_body,
                                                    const std::string& b_body) {
    const auto a = parse(slide_xml(a_body));
    const auto b = parse(slide_xml(b_body));
    return pptctl::compare_slides(a, b, {}).pictures;
}

// The loaded slide `n` (1-based) of a fixture deck. Both the package and the
// presentation are kept alive by the caller.
pptctl::Slide load_slide(const pptctl::Package& pkg,
                         const pptctl::Presentation& pres,
                         std::size_t n) {
    pptctl::Slide s;
    REQUIRE(s.load(pkg, pres.slide_parts().at(n - 1)));
    s.set_index(n);
    return s;
}

struct OpenDeck {
    pptctl::Package pkg;
    pptctl::Presentation pres{pkg};

    explicit OpenDeck(const char* name) {
        REQUIRE(pkg.open(fixture(name)));
        REQUIRE(pres.load());
    }
};

}  // namespace

// ── unit cases on inline XML (no package: identity falls back to the rel target)

TEST_CASE("Diff/picture: identical slides yield no picture ops") {
    const auto body = pic("2", "Picture 1", "rId2");
    CHECK(pictures_between(body, body).empty());
}

TEST_CASE("Diff/picture: a picture only in B is Added") {
    const auto ops = pictures_between("", pic("2", "Picture 1", "rId2"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Added);
    CHECK(ops[0].shape_id == "2");
    CHECK(ops[0].shape_name == "Picture 1");
    CHECK_FALSE(ops[0].before.has_value());
    REQUIRE(ops[0].after.has_value());
    CHECK(ops[0].after->rel_id == "rId2");
}

TEST_CASE("Diff/picture: a picture only in A is Removed") {
    const auto ops = pictures_between(pic("2", "Picture 1", "rId2"), "");
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Removed);
    CHECK(ops[0].shape_id == "2");
    REQUIRE(ops[0].before.has_value());
    CHECK_FALSE(ops[0].after.has_value());
}

TEST_CASE("Diff/picture: same shape pointing at different media is Replaced") {
    const auto ops = pictures_between(pic("2", "Picture 1", "rId2"), pic("2", "Picture 1", "rId3"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Replaced);
    REQUIRE(ops[0].before.has_value());
    REQUIRE(ops[0].after.has_value());
    CHECK(ops[0].before->rel_id == "rId2");
    CHECK(ops[0].after->rel_id == "rId3");
}

TEST_CASE("Diff/picture: embed <-> link is Relinked, not Replaced") {
    const auto ops =
        pictures_between(pic("2", "Picture 1", "rId2"), pic("2", "Picture 1", "rId2", true));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Relinked);
    CHECK_FALSE(ops[0].before->linked);
    CHECK(ops[0].after->linked);

    const auto back =
        pictures_between(pic("2", "Picture 1", "rId2", true), pic("2", "Picture 1", "rId2"));
    REQUIRE(back.size() == 1);
    CHECK(back[0].op == pptctl::PictureOp::Relinked);
}

TEST_CASE("Diff/picture: alt text, title and decorative changes are AltTextChanged") {
    SUBCASE("descr") {
        const auto ops = pictures_between(pic("2", "Picture 1", "rId2", false, "descr=\"A cat\""),
                                          pic("2", "Picture 1", "rId2", false, "descr=\"A dog\""));
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].op == pptctl::PictureOp::AltTextChanged);
        CHECK(ops[0].before->alt_text == "A cat");
        CHECK(ops[0].after->alt_text == "A dog");
    }
    SUBCASE("descr added") {
        const auto ops = pictures_between(pic("2", "Picture 1", "rId2"),
                                          pic("2", "Picture 1", "rId2", false, "descr=\"A dog\""));
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].op == pptctl::PictureOp::AltTextChanged);
        CHECK_FALSE(ops[0].before->alt_text.has_value());
    }
    SUBCASE("title") {
        const auto ops = pictures_between(pic("2", "Picture 1", "rId2", false, "title=\"One\""),
                                          pic("2", "Picture 1", "rId2", false, "title=\"Two\""));
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].op == pptctl::PictureOp::AltTextChanged);
    }
}

TEST_CASE("Diff/picture: a replaced picture whose alt text also changed yields both ops") {
    const auto ops = pictures_between(pic("2", "Picture 1", "rId2", false, "descr=\"old\""),
                                      pic("2", "Picture 1", "rId3", false, "descr=\"new\""));
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == pptctl::PictureOp::Replaced);
    CHECK(ops[1].op == pptctl::PictureOp::AltTextChanged);
}

TEST_CASE("Diff/picture: pictures match by shape id, then by name") {
    SUBCASE("id changed, name kept -> matched") {
        const auto ops = pictures_between(pic("2", "Logo", "rId2"), pic("7", "Logo", "rId2"));
        CHECK(ops.empty());
    }
    SUBCASE("id changed, name kept, media changed -> Replaced") {
        const auto ops = pictures_between(pic("2", "Logo", "rId2"), pic("7", "Logo", "rId9"));
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].op == pptctl::PictureOp::Replaced);
        CHECK(ops[0].shape_id == "7");  // the B side's id names the shape
    }
    SUBCASE("id and name both changed -> Removed + Added") {
        const auto ops = pictures_between(pic("2", "Logo", "rId2"), pic("7", "Photo", "rId2"));
        REQUIRE(ops.size() == 2);
        CHECK(ops[0].op == pptctl::PictureOp::Removed);
        CHECK(ops[1].op == pptctl::PictureOp::Added);
    }
}

TEST_CASE("Diff/picture: pictures nested in groups are compared") {
    const auto grouped = [](const char* rid) {
        return std::string(
                   "\n    <p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"5\" name=\"Group 5\"/>"
                   "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>") +
               pic("6", "Inner", rid) + "</p:grpSp>";
    };
    const auto ops = pictures_between(grouped("rId2"), grouped("rId3"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Replaced);
    CHECK(ops[0].shape_id == "6");
}

TEST_CASE("Diff/picture: non-picture shapes never produce picture ops") {
    const std::string box =
        "\n    <p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"TextBox 2\"/><p:cNvSpPr/><p:nvPr/>"
        "</p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>hi</a:t></a:r></a:p>"
        "</p:txBody></p:sp>";
    CHECK(pictures_between(box, "").empty());
    CHECK(pictures_between("", box).empty());
}

// ── describe / to_json ───────────────────────────────────────────────────────

TEST_CASE("Diff/picture: describe is a one-liner naming the shape and the op") {
    const auto ops = pictures_between(pic("2", "Picture 1", "rId2"), pic("2", "Picture 1", "rId3"));
    REQUIRE(ops.size() == 1);
    const auto line = pptctl::describe(ops[0]);
    CHECK(line.find("picture \"Picture 1\" replaced") == 0);
    CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("Diff/picture: to_json uses stable snake_case keys") {
    const auto ops = pictures_between(pic("2", "Picture 1", "rId2", false, "descr=\"old\""),
                                      pic("2", "Picture 1", "rId2", false, "descr=\"new\""));
    REQUIRE(ops.size() == 1);
    const auto j = pptctl::to_json(ops[0]);
    CHECK(j["op"] == "alt_text_changed");
    CHECK(j["shape_id"] == "2");
    CHECK(j["shape_name"] == "Picture 1");
    REQUIRE(j.contains("before"));
    REQUIRE(j.contains("after"));
    CHECK(j["before"]["alt_text"] == "old");
    CHECK(j["after"]["alt_text"] == "new");
    CHECK(j["before"]["linked"] == false);
    CHECK(j["before"]["media_hash"].is_null());  // no package: unresolved
    CHECK(j["before"]["size_bytes"].is_null());
    CHECK(j["before"]["width"].is_null());

    const auto added = pictures_between("", pic("2", "Picture 1", "rId2"));
    const auto ja = pptctl::to_json(added.at(0));
    CHECK(ja["op"] == "added");
    CHECK(ja["before"].is_null());
    CHECK(ja["after"]["rel_id"] == "rId2");
}

// ── fixture cases: images_deck.pptx vs images_deck_v2.pptx ───────────────────
//
// v2 renames every media part (image1.png -> picture1.png), so nothing below
// can pass on part names: it has to go through the media hash.

TEST_CASE("Diff/picture: same media under a different part name is not reported") {
    OpenDeck a("images_deck.pptx");
    OpenDeck b("images_deck_v2.pptx");
    const auto sa = load_slide(a.pkg, a.pres, 1);
    const auto sb = load_slide(b.pkg, b.pres, 1);

    // Sanity: the fixture really does rename the part.
    const pptctl::Shape* pa = nullptr;
    const pptctl::Shape* pb = nullptr;
    for (const auto& s : sa.shapes()) {
        if (s.picture) {
            pa = &s;
        }
    }
    for (const auto& s : sb.shapes()) {
        if (s.picture) {
            pb = &s;
        }
    }
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(pa->picture->target == "ppt/media/image1.png");
    CHECK(pb->picture->target == "ppt/media/picture1.png");
    REQUIRE(pa->picture->media_hash.has_value());
    CHECK(pa->picture->media_hash == pb->picture->media_hash);

    CHECK(pptctl::compare_slides(sa, sb, {}).pictures.empty());
    CHECK(pptctl::summarize(sa).fingerprint == pptctl::summarize(sb).fingerprint);
}

TEST_CASE("Diff/picture: a swapped image under the same shape is Replaced, with dimensions") {
    OpenDeck a("images_deck.pptx");
    OpenDeck b("images_deck_v2.pptx");
    const auto sa = load_slide(a.pkg, a.pres, 3);
    const auto sb = load_slide(b.pkg, b.pres, 3);

    const auto ops = pptctl::compare_slides(sa, sb, {}).pictures;
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Replaced);
    REQUIRE(ops[0].before.has_value());
    REQUIRE(ops[0].after.has_value());
    CHECK(ops[0].before->target == "ppt/media/image3.png");
    CHECK(ops[0].after->target == "ppt/media/picture3.png");
    CHECK(ops[0].before->media_hash != ops[0].after->media_hash);
    REQUIRE(ops[0].before->dimensions.has_value());
    CHECK(ops[0].before->dimensions->width == 66);
    CHECK(ops[0].before->dimensions->height == 50);
    REQUIRE(ops[0].after->dimensions.has_value());
    CHECK(ops[0].after->dimensions->width == 66);
    CHECK(ops[0].before->size_bytes.has_value());
    CHECK(ops[0].after->size_bytes.has_value());

    const auto line = pptctl::describe(ops[0]);
    CHECK(line.find("image3.png") != std::string::npos);
    CHECK(line.find("picture3.png") != std::string::npos);
    CHECK(line.find("66x50") != std::string::npos);

    const auto j = pptctl::to_json(ops[0]);
    CHECK(j["op"] == "replaced");
    CHECK(j["before"]["width"] == 66);
    CHECK(j["before"]["height"] == 50);
    CHECK(j["before"]["media_hash"].is_string());
    CHECK(j["before"]["media_hash"] != j["after"]["media_hash"]);
    CHECK(j["before"]["content_type"] == "image/png");

    // The fingerprint sees the swap too, so slide matching reports Changed.
    CHECK(pptctl::summarize(sa).fingerprint != pptctl::summarize(sb).fingerprint);
}

TEST_CASE("Diff/picture: end to end over the fixture pair") {
    OpenDeck a("images_deck.pptx");
    OpenDeck b("images_deck_v2.pptx");
    std::vector<pptctl::SlideSummary> sa;
    std::vector<pptctl::SlideSummary> sb;
    for (std::size_t i = 1; i <= 12; ++i) {
        sa.push_back(pptctl::summarize(load_slide(a.pkg, a.pres, i)));
        sb.push_back(pptctl::summarize(load_slide(b.pkg, b.pres, i)));
    }
    const auto report = pptctl::match_slides(sa, sb, {});
    REQUIRE(report.slides.size() == 12);

    std::vector<pptctl::PictureOp> ops;
    for (const auto& s : report.slides) {
        CHECK(s.a_index == s.b_index);
        CHECK_FALSE(s.moved);
        const bool edited = s.b_index == 3 || s.b_index == 5 || s.b_index == 7 || s.b_index == 9;
        CHECK(s.status == (edited ? pptctl::SlideStatus::Changed : pptctl::SlideStatus::Unchanged));
        if (s.status != pptctl::SlideStatus::Changed) {
            continue;
        }
        const auto changes = pptctl::compare_slides(
            load_slide(a.pkg, a.pres, s.a_index), load_slide(b.pkg, b.pres, s.b_index), {});
        REQUIRE(changes.pictures.size() == 1);
        ops.push_back(changes.pictures[0].op);
    }
    REQUIRE(ops.size() == 4);
    CHECK(ops[0] == pptctl::PictureOp::Replaced);
    CHECK(ops[1] == pptctl::PictureOp::Removed);
    CHECK(ops[2] == pptctl::PictureOp::Added);
    CHECK(ops[3] == pptctl::PictureOp::AltTextChanged);
}

// ── slide matching (#121) ─────────────────────────────────────────────────────

namespace {

// A p:pic with its own frame, so slide matching can recognise the picture's slot.
std::string framed_pic(const char* rid, const char* x = "914400") {
    return std::string(R"(
    <p:pic><p:nvPicPr><p:cNvPr id="2" name="Picture 1"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:embed=")") +
           rid + R"("/></p:blipFill>
      <p:spPr><a:xfrm><a:off x=")" +
           x + R"(" y="914400"/><a:ext cx="1828800" cy="1371600"/></a:xfrm></p:spPr></p:pic>)";
}

pptctl::DiffReport match_one(const std::string& a_body, const std::string& b_body) {
    auto a = parse(slide_xml(a_body));
    auto b = parse(slide_xml(b_body));
    a.set_index(1);
    b.set_index(1);
    return pptctl::match_slides({pptctl::summarize(a)}, {pptctl::summarize(b)}, {});
}

}  // namespace

TEST_CASE("Diff/picture: a picture-only slide whose image changed pairs as Changed") {
    // Parse-only slides identify a picture by its rId: rId3 stands in for new
    // bytes, as when optimize re-encodes the only image on a slide.
    const auto report = match_one(framed_pic("rId2"), framed_pic("rId3"));
    REQUIRE(report.slides.size() == 1);
    CHECK(report.slides[0].status == pptctl::SlideStatus::Changed);
    CHECK(report.slides[0].a_index == 1);
    CHECK(report.slides[0].b_index == 1);

    const auto ops = pictures_between(framed_pic("rId2"), framed_pic("rId3"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::PictureOp::Replaced);
}

TEST_CASE("Diff/picture: a picture moved and swapped is a different slide") {
    const auto report = match_one(framed_pic("rId2"), framed_pic("rId3", "5000000"));
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[0].status == pptctl::SlideStatus::Removed);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Added);
}
