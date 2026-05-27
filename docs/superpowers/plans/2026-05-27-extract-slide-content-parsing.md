# extract — slide content parsing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Walk a slide's shape tree (`p:spTree`) and pull out its text structure — shapes, group shapes, placeholders, text frames, paragraphs, runs — preserving reading order, and expose it through a minimal `pptctl extract` text dump.

**Architecture:** A new document-layer unit `slide.hpp`/`slide.cpp` (alongside `presentation.hpp`) parses one slide's XML into owned value structs (`Slide` → `Shape` → `Paragraph` → `Run`). pugixml stays internal to the `.cpp`. Parsing is split behind a `Slide::parse(xml)` seam so the parser is unit-testable from raw XML strings, while `Slide::load(Package&, part)` is the production entry. `Presentation` gains an ordered `slide_parts()` list so the command loads slides lazily, one at a time.

**Tech Stack:** C++20, pugixml (XML), doctest (tests), CLI11 (CLI), CMake + Ninja.

**Spec:** `docs/superpowers/specs/2026-05-27-extract-slide-content-parsing-design.md`

**Build/test reference:**
- Full configure + build + test: `./scripts/build.sh`
- Build only (after a configure): `cmake --build --preset macos-release --parallel`
- Reconfigure (needed after editing `CMakeLists.txt`): `cmake --preset macos-release`
- Run one doctest case: `./build/macos-release/pptctl_tests --test-case="<name>"`
- Run all slide tests: `./build/macos-release/pptctl_tests --test-case="Slide:*"`

Run `clang-format -i` on any file you create or edit before committing.

---

## Task 1: Scaffold the data model, parser stub, and build wiring

**Files:**
- Create: `include/pptctl/slide.hpp`
- Create: `src/lib/slide.cpp`
- Create: `tests/test_slide.cpp`
- Modify: `CMakeLists.txt` (add `src/lib/slide.cpp` to `pptctl_lib`; add `tests/test_slide.cpp` to `pptctl_tests`)

- [ ] **Step 1: Write the data model header**

Create `include/pptctl/slide.hpp`:

```cpp
// pptctl/slide.hpp
//
// Document layer: the content of a single slide — its shape tree, text
// frames, paragraphs, and runs. Sits on top of the package layer, alongside
// presentation.hpp. Read-only: this layer never serializes slides back.

#pragma once

#include <cstddef>
#include <optional>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>  // for Emu
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

enum class ShapeKind {
    Placeholder,
    TextBox,
    Picture,
    Table,
    Chart,
    Diagram,
    Ole,
    Group,
    Connector,
    Other,
};

// Run-level formatting from a:rPr. Each field is set only when the source
// attribute is present, so "unset" (inherit) stays distinct from "explicitly
// off".
struct RunProperties {
    std::optional<bool> bold;         // a:rPr@b
    std::optional<bool> italic;       // a:rPr@i
    std::optional<bool> underlined;   // a:rPr@u != "none"
    std::optional<int> size_centipt;  // a:rPr@sz, hundredths of a point (native)
    std::optional<std::string> font;  // a:rPr/a:latin@typeface

    // Convenience; mirrors the EMU->inches pattern. 0 when size is unset.
    [[nodiscard]] double size_pt() const noexcept {
        return size_centipt ? static_cast<double>(*size_centipt) / 100.0 : 0.0;
    }
};

struct Run {
    std::string text;
    RunProperties props;
    bool is_field = false;  // came from a:fld rather than a:r
};

struct Paragraph {
    int level = 0;  // a:pPr@lvl (bullet / indent depth)
    std::vector<Run> runs;

    [[nodiscard]] std::string text() const {
        std::string out;
        for (const auto& r : runs) {
            out += r.text;
        }
        return out;
    }
};

// Present only when the shape carries an a:xfrm. EMU, the native OOXML unit.
struct Geometry {
    Emu off_x = 0;
    Emu off_y = 0;
    Emu ext_cx = 0;
    Emu ext_cy = 0;
};

struct Shape {
    ShapeKind kind = ShapeKind::Other;
    std::string id;                              // p:cNvPr@id
    std::string name;                            // p:cNvPr@name
    std::optional<std::string> placeholder_type;  // p:nvPr/p:ph@type
    std::optional<Geometry> geometry;
    std::vector<Paragraph> paragraphs;  // text body; empty for non-text shapes
    std::vector<Shape> children;        // populated only for Group, recursive
};

class Slide {
 public:
    Slide() = default;

    // Read and parse a slide part from the package. Returns false if the part
    // is missing or not parseable.
    [[nodiscard]] bool load(const Package& pkg, std::string_view part_name);

    // Parse slide XML directly from bytes. Exposed for testing the parser
    // without a package. Returns false if the XML can't be parsed.
    [[nodiscard]] bool parse(std::string_view xml);

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }
    [[nodiscard]] const std::string& part_name() const noexcept {
        return part_name_;
    }
    [[nodiscard]] const std::vector<Shape>& shapes() const noexcept {
        return shapes_;
    }

    void set_index(std::size_t i) noexcept {
        index_ = i;
    }

 private:
    std::size_t index_ = 0;
    std::string part_name_;
    std::vector<Shape> shapes_;
};

}  // namespace pptctl
```

- [ ] **Step 2: Write the parser stub**

Create `src/lib/slide.cpp`:

```cpp
// pptctl/slide.cpp — parses a slide's shape tree into a content model.

#include <pptctl/slide.hpp>
#include <pugixml.hpp>

namespace pptctl {

bool Slide::parse(std::string_view xml) {
    shapes_.clear();
    pugi::xml_document doc;
    const auto result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        return false;
    }
    return true;
}

bool Slide::load(const Package& pkg, std::string_view part_name) {
    part_name_ = std::string(part_name);
    const auto bytes = pkg.read_part(part_name);
    if (bytes.empty()) {
        return false;
    }
    return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}  // namespace pptctl
```

- [ ] **Step 3: Write the first test (default + empty parse)**

Create `tests/test_slide.cpp`:

```cpp
// Tests for the Slide content model (shape tree, text frames, paragraphs, runs).

#include <doctest/doctest.h>

#include <pptctl/slide.hpp>
#include <string>

namespace {

// A minimal slide with an empty shape tree.
constexpr const char* kEmptySlide = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
  </p:spTree></p:cSld>
</p:sld>)";

}  // namespace

TEST_CASE("Slide: default-constructed slide has no shapes") {
    pptctl::Slide slide;
    CHECK(slide.shapes().empty());
    CHECK(slide.index() == 0);
}

TEST_CASE("Slide: parse of malformed XML returns false") {
    pptctl::Slide slide;
    CHECK_FALSE(slide.parse("<not-closed"));
}

TEST_CASE("Slide: parse of an empty shape tree yields no shapes") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kEmptySlide));
    CHECK(slide.shapes().empty());
}
```

- [ ] **Step 4: Wire the new files into CMake**

In `CMakeLists.txt`, add `src/lib/slide.cpp` to the `pptctl_lib` source list (currently lines ~112-115):

```cmake
add_library(pptctl_lib STATIC
    src/lib/package.cpp
    src/lib/presentation.cpp
    src/lib/slide.cpp
)
```

And add `tests/test_slide.cpp` to the `pptctl_tests` source list (currently lines ~140-143):

```cmake
add_executable(pptctl_tests
    tests/test_main.cpp
    tests/test_package.cpp
    tests/test_slide.cpp
)
```

- [ ] **Step 5: Configure, build, and run the new tests**

Run: `cmake --preset macos-release && cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: builds cleanly (no warnings), 3 Slide test cases pass.

- [ ] **Step 6: Commit**

```bash
clang-format -i include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp
git add include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp CMakeLists.txt
git commit -m "feat(extract): scaffold slide content model and parser (#7)"
```

---

## Task 2: Parse a single text shape into paragraphs and runs

**Files:**
- Modify: `src/lib/slide.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_slide.cpp` (inside the anonymous namespace, add the fixture; add the TEST_CASE after it):

```cpp
// One placeholder shape with a two-run title paragraph.
constexpr const char* kOneTextShape = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Title 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/><a:lstStyle/>
        <a:p>
          <a:r><a:t>Hello </a:t></a:r>
          <a:r><a:t>World</a:t></a:r>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
```

```cpp
TEST_CASE("Slide: parses a single text shape into paragraphs and runs") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kOneTextShape));
    REQUIRE(slide.shapes().size() == 1);

    const auto& sp = slide.shapes()[0];
    CHECK(sp.id == "2");
    CHECK(sp.name == "Title 1");
    REQUIRE(sp.paragraphs.size() == 1);
    REQUIRE(sp.paragraphs[0].runs.size() == 2);
    CHECK(sp.paragraphs[0].runs[0].text == "Hello ");
    CHECK(sp.paragraphs[0].runs[1].text == "World");
    CHECK(sp.paragraphs[0].text() == "Hello World");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build/macos-release/pptctl_tests --test-case="Slide: parses a single text shape*"`
Expected: FAIL — `shapes().size()` is 0, not 1.

- [ ] **Step 3: Implement shape-tree traversal and text-body parsing**

Replace the body of `Slide::parse` in `src/lib/slide.cpp`, adding an anonymous-namespace helper section above it. Full file content for `src/lib/slide.cpp`:

```cpp
// pptctl/slide.cpp — parses a slide's shape tree into a content model.

#include <pptctl/slide.hpp>
#include <pugixml.hpp>
#include <string_view>

namespace pptctl {

namespace {

// The shape's identifying p:cNvPr lives inside the shape's first child (the
// nv* container: p:nvSpPr / p:nvPicPr / p:nvGrpSpPr / etc.), regardless of
// shape kind.
pugi::xml_node nv_cnvpr(pugi::xml_node shape_el) {
    return shape_el.first_child().child("p:cNvPr");
}

void parse_text_body(pugi::xml_node tx_body, Shape& shape) {
    for (auto p : tx_body.children("a:p")) {
        Paragraph para;
        para.level = p.child("a:pPr").attribute("lvl").as_int(0);
        for (auto node : p.children()) {
            const std::string_view name = node.name();
            if (name == "a:r") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                para.runs.push_back(std::move(run));
            }
        }
        shape.paragraphs.push_back(std::move(para));
    }
}

Shape parse_sp(pugi::xml_node sp_el) {
    Shape shape;
    shape.kind = ShapeKind::TextBox;
    auto cnvpr = nv_cnvpr(sp_el);
    shape.id = cnvpr.attribute("id").as_string("");
    shape.name = cnvpr.attribute("name").as_string("");
    if (auto tx = sp_el.child("p:txBody")) {
        parse_text_body(tx, shape);
    }
    return shape;
}

void parse_sp_tree(pugi::xml_node tree, std::vector<Shape>& out) {
    for (auto el : tree.children()) {
        const std::string_view name = el.name();
        if (name == "p:sp") {
            out.push_back(parse_sp(el));
        }
    }
}

}  // namespace

bool Slide::parse(std::string_view xml) {
    shapes_.clear();
    pugi::xml_document doc;
    const auto result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        return false;
    }
    auto tree = doc.child("p:sld").child("p:cSld").child("p:spTree");
    if (!tree) {
        return false;
    }
    parse_sp_tree(tree, shapes_);
    return true;
}

bool Slide::load(const Package& pkg, std::string_view part_name) {
    part_name_ = std::string(part_name);
    const auto bytes = pkg.read_part(part_name);
    if (bytes.empty()) {
        return false;
    }
    return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}  // namespace pptctl
```

Note: the `kEmptySlide` test still passes because `p:spTree` exists but has no `p:sp` children.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: all Slide cases PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): parse text shapes into paragraphs and runs (#7)"
```

---

## Task 3: Parse run properties (bold, italic, underline, size, font)

**Files:**
- Modify: `src/lib/slide.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add the fixture and test to `tests/test_slide.cpp`:

```cpp
// A run with explicit formatting, plus a bare run (no rPr) to check "unset".
constexpr const char* kFormattedRuns = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="TextBox 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/>
        <a:p>
          <a:r>
            <a:rPr b="1" i="0" u="sng" sz="2400"><a:latin typeface="Arial"/></a:rPr>
            <a:t>Bold</a:t>
          </a:r>
          <a:r><a:t>Plain</a:t></a:r>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
```

```cpp
TEST_CASE("Slide: parses run properties and leaves unset fields empty") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kFormattedRuns));
    REQUIRE(slide.shapes().size() == 1);
    const auto& runs = slide.shapes()[0].paragraphs.at(0).runs;
    REQUIRE(runs.size() == 2);

    const auto& fmt = runs[0].props;
    REQUIRE(fmt.bold.has_value());
    CHECK(*fmt.bold == true);
    REQUIRE(fmt.italic.has_value());
    CHECK(*fmt.italic == false);
    REQUIRE(fmt.underlined.has_value());
    CHECK(*fmt.underlined == true);
    REQUIRE(fmt.size_centipt.has_value());
    CHECK(*fmt.size_centipt == 2400);
    CHECK(fmt.size_pt() == doctest::Approx(24.0));
    REQUIRE(fmt.font.has_value());
    CHECK(*fmt.font == "Arial");

    // Bare run: every property unset.
    const auto& bare = runs[1].props;
    CHECK_FALSE(bare.bold.has_value());
    CHECK_FALSE(bare.italic.has_value());
    CHECK_FALSE(bare.underlined.has_value());
    CHECK_FALSE(bare.size_centipt.has_value());
    CHECK_FALSE(bare.font.has_value());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build/macos-release/pptctl_tests --test-case="Slide: parses run properties*"`
Expected: FAIL — `fmt.bold.has_value()` is false (props not parsed yet).

- [ ] **Step 3: Implement run-property parsing**

In `src/lib/slide.cpp`, add a helper in the anonymous namespace (above `parse_text_body`):

```cpp
RunProperties parse_run_props(pugi::xml_node rpr) {
    RunProperties props;
    if (!rpr) {
        return props;
    }
    if (auto a = rpr.attribute("b")) {
        props.bold = a.as_bool();
    }
    if (auto a = rpr.attribute("i")) {
        props.italic = a.as_bool();
    }
    if (auto a = rpr.attribute("u")) {
        props.underlined = (std::string_view(a.as_string("")) != "none");
    }
    if (auto a = rpr.attribute("sz")) {
        props.size_centipt = a.as_int();
    }
    if (auto latin = rpr.child("a:latin")) {
        if (auto tf = latin.attribute("typeface")) {
            props.font = tf.as_string("");
        }
    }
    return props;
}
```

Then in `parse_text_body`, set the run's props when handling `a:r`:

```cpp
            if (name == "a:r") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                run.props = parse_run_props(node.child("a:rPr"));
                para.runs.push_back(std::move(run));
            }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: all Slide cases PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): capture run formatting (bold/italic/underline/size/font) (#7)"
```

---

## Task 4: Paragraph levels, empty paragraphs, fields, and line breaks

**Files:**
- Modify: `src/lib/slide.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add the fixture and test to `tests/test_slide.cpp`:

```cpp
// Bulleted paragraphs at two levels, an empty paragraph, a field, and a break.
constexpr const char* kRichParagraphs = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Body 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/>
        <a:p><a:r><a:t>Top</a:t></a:r></a:p>
        <a:p><a:pPr lvl="1"/><a:r><a:t>Nested</a:t></a:r></a:p>
        <a:p/>
        <a:p>
          <a:r><a:t>Line one</a:t></a:r>
          <a:br/>
          <a:fld id="{GUID}" type="slidenum"><a:t>3</a:t></a:fld>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
```

```cpp
TEST_CASE("Slide: parses levels, empty paragraphs, fields, and breaks") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kRichParagraphs));
    REQUIRE(slide.shapes().size() == 1);
    const auto& paras = slide.shapes()[0].paragraphs;
    REQUIRE(paras.size() == 4);

    CHECK(paras[0].level == 0);
    CHECK(paras[0].text() == "Top");

    CHECK(paras[1].level == 1);
    CHECK(paras[1].text() == "Nested");

    // Empty paragraph preserved as structure.
    CHECK(paras[2].runs.empty());
    CHECK(paras[2].text().empty());

    // Run, break (newline), then a field run — in document order.
    REQUIRE(paras[3].runs.size() == 3);
    CHECK(paras[3].runs[0].text == "Line one");
    CHECK_FALSE(paras[3].runs[0].is_field);
    CHECK(paras[3].runs[1].text == "\n");
    CHECK(paras[3].runs[2].text == "3");
    CHECK(paras[3].runs[2].is_field);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build/macos-release/pptctl_tests --test-case="Slide: parses levels*"`
Expected: FAIL — paragraph 3 has 1 run (only `a:r`), breaks and fields ignored.

- [ ] **Step 3: Implement field and break handling**

In `parse_text_body` in `src/lib/slide.cpp`, extend the per-node dispatch inside the `for (auto node : p.children())` loop:

```cpp
        for (auto node : p.children()) {
            const std::string_view name = node.name();
            if (name == "a:r") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                run.props = parse_run_props(node.child("a:rPr"));
                para.runs.push_back(std::move(run));
            } else if (name == "a:fld") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                run.props = parse_run_props(node.child("a:rPr"));
                run.is_field = true;
                para.runs.push_back(std::move(run));
            } else if (name == "a:br") {
                Run run;
                run.text = "\n";
                para.runs.push_back(std::move(run));
            }
        }
```

(Paragraph `level` and empty-paragraph handling already work from Task 2.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: all Slide cases PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): handle paragraph levels, fields, and line breaks (#7)"
```

---

## Task 5: Shape kinds, placeholder type, and geometry

**Files:**
- Modify: `src/lib/slide.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add the fixture and test to `tests/test_slide.cpp`:

```cpp
// One placeholder (with type + xfrm), a picture, a table, a chart, and a
// connector — to exercise kind detection, placeholder type, and geometry.
constexpr const char* kMixedShapes = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Title 1"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="ctrTitle"/></p:nvPr></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="685800" y="2130425"/><a:ext cx="7772400" cy="2898775"/></a:xfrm></p:spPr>
      <p:txBody><a:bodyPr/><a:p><a:r><a:t>Title</a:t></a:r></a:p></p:txBody>
    </p:sp>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="3" name="Picture 3"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill/><p:spPr/>
    </p:pic>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="4" name="Table 4"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"/></a:graphic>
    </p:graphicFrame>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="5" name="Chart 5"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart"/></a:graphic>
    </p:graphicFrame>
    <p:cxnSp>
      <p:nvCxnSpPr><p:cNvPr id="6" name="Connector 6"/></p:nvCxnSpPr>
      <p:spPr/>
    </p:cxnSp>
  </p:spTree></p:cSld>
</p:sld>)";
```

```cpp
TEST_CASE("Slide: detects shape kinds, placeholder type, and geometry") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kMixedShapes));
    REQUIRE(slide.shapes().size() == 5);
    const auto& s = slide.shapes();

    CHECK(s[0].kind == pptctl::ShapeKind::Placeholder);
    REQUIRE(s[0].placeholder_type.has_value());
    CHECK(*s[0].placeholder_type == "ctrTitle");
    REQUIRE(s[0].geometry.has_value());
    CHECK(s[0].geometry->off_x == 685800);
    CHECK(s[0].geometry->ext_cy == 2898775);

    CHECK(s[1].kind == pptctl::ShapeKind::Picture);
    CHECK(s[1].name == "Picture 3");
    CHECK_FALSE(s[1].geometry.has_value());  // no a:xfrm

    CHECK(s[2].kind == pptctl::ShapeKind::Table);
    CHECK(s[3].kind == pptctl::ShapeKind::Chart);
    CHECK(s[4].kind == pptctl::ShapeKind::Connector);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build/macos-release/pptctl_tests --test-case="Slide: detects shape kinds*"`
Expected: FAIL — only the `p:sp` is parsed; `shapes().size()` is 1, not 5.

- [ ] **Step 3: Implement kind detection, placeholder type, and geometry**

In `src/lib/slide.cpp`, add helpers in the anonymous namespace (above `parse_sp`):

```cpp
std::optional<Geometry> parse_geometry(pugi::xml_node shape_el) {
    // a:xfrm lives under p:spPr (sp/pic/cxnSp), p:grpSpPr (group), or directly
    // under a graphicFrame (p:xfrm).
    pugi::xml_node xfrm = shape_el.child("p:spPr").child("a:xfrm");
    if (!xfrm) {
        xfrm = shape_el.child("p:grpSpPr").child("a:xfrm");
    }
    if (!xfrm) {
        xfrm = shape_el.child("p:xfrm");
    }
    if (!xfrm) {
        return std::nullopt;
    }
    Geometry g;
    auto off = xfrm.child("a:off");
    auto ext = xfrm.child("a:ext");
    g.off_x = off.attribute("x").as_llong(0);
    g.off_y = off.attribute("y").as_llong(0);
    g.ext_cx = ext.attribute("cx").as_llong(0);
    g.ext_cy = ext.attribute("cy").as_llong(0);
    return g;
}

void set_identity(pugi::xml_node shape_el, Shape& shape) {
    auto cnvpr = nv_cnvpr(shape_el);
    shape.id = cnvpr.attribute("id").as_string("");
    shape.name = cnvpr.attribute("name").as_string("");
    shape.geometry = parse_geometry(shape_el);
}

ShapeKind graphic_frame_kind(pugi::xml_node gf) {
    const std::string_view uri =
        gf.child("a:graphic").child("a:graphicData").attribute("uri").as_string("");
    if (uri.find("table") != std::string_view::npos) {
        return ShapeKind::Table;
    }
    if (uri.find("chart") != std::string_view::npos) {
        return ShapeKind::Chart;
    }
    if (uri.find("diagram") != std::string_view::npos) {
        return ShapeKind::Diagram;
    }
    if (uri.find("ole") != std::string_view::npos) {
        return ShapeKind::Ole;
    }
    return ShapeKind::Other;
}
```

Replace `parse_sp` so it sets identity/geometry, the placeholder kind/type, and parses text:

```cpp
Shape parse_sp(pugi::xml_node sp_el) {
    Shape shape;
    set_identity(sp_el, shape);
    auto ph = sp_el.child("p:nvSpPr").child("p:nvPr").child("p:ph");
    if (ph) {
        shape.kind = ShapeKind::Placeholder;
        if (auto type = ph.attribute("type")) {
            shape.placeholder_type = type.as_string("");
        }
    } else {
        shape.kind = ShapeKind::TextBox;
    }
    if (auto tx = sp_el.child("p:txBody")) {
        parse_text_body(tx, shape);
    }
    return shape;
}
```

Then extend `parse_sp_tree` to dispatch the remaining kinds:

```cpp
void parse_sp_tree(pugi::xml_node tree, std::vector<Shape>& out) {
    for (auto el : tree.children()) {
        const std::string_view name = el.name();
        if (name == "p:sp") {
            out.push_back(parse_sp(el));
        } else if (name == "p:pic") {
            Shape shape;
            shape.kind = ShapeKind::Picture;
            set_identity(el, shape);
            out.push_back(std::move(shape));
        } else if (name == "p:graphicFrame") {
            Shape shape;
            shape.kind = graphic_frame_kind(el);
            set_identity(el, shape);
            out.push_back(std::move(shape));
        } else if (name == "p:cxnSp") {
            Shape shape;
            shape.kind = ShapeKind::Connector;
            set_identity(el, shape);
            out.push_back(std::move(shape));
        }
    }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: all Slide cases PASS (the Task 2 `kOneTextShape` shape is now kind `TextBox` — still has id "2"/name "Title 1", so those assertions hold).

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): detect shape kinds, placeholder type, and geometry (#7)"
```

---

## Task 6: Group shape recursion and reading order

**Files:**
- Modify: `src/lib/slide.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add the fixture and test to `tests/test_slide.cpp`:

```cpp
// A top-level shape, then a group containing two shapes — to check recursion
// and that document order is preserved at both levels.
constexpr const char* kGroupedShapes = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Outer"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:p><a:r><a:t>Outer</a:t></a:r></a:p></p:txBody>
    </p:sp>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="3" name="Group 3"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr/>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="4" name="Inner A"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/><a:p><a:r><a:t>A</a:t></a:r></a:p></p:txBody>
      </p:sp>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="5" name="Inner B"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/><a:p><a:r><a:t>B</a:t></a:r></a:p></p:txBody>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";
```

```cpp
TEST_CASE("Slide: recurses into group shapes preserving reading order") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kGroupedShapes));
    REQUIRE(slide.shapes().size() == 2);

    CHECK(slide.shapes()[0].name == "Outer");

    const auto& group = slide.shapes()[1];
    CHECK(group.kind == pptctl::ShapeKind::Group);
    CHECK(group.name == "Group 3");
    REQUIRE(group.children.size() == 2);
    CHECK(group.children[0].name == "Inner A");
    CHECK(group.children[0].paragraphs.at(0).text() == "A");
    CHECK(group.children[1].name == "Inner B");
    CHECK(group.children[1].paragraphs.at(0).text() == "B");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build/macos-release/pptctl_tests --test-case="Slide: recurses into group*"`
Expected: FAIL — `shapes().size()` is 1 (the group is not yet handled).

- [ ] **Step 3: Implement group recursion**

In `src/lib/slide.cpp`, add a `p:grpSp` branch to `parse_sp_tree`. Because `parse_sp_tree` recurses, move its declaration above any caller is unnecessary (it already exists); just add the branch:

```cpp
        } else if (name == "p:grpSp") {
            Shape shape;
            shape.kind = ShapeKind::Group;
            set_identity(el, shape);
            parse_sp_tree(el, shape.children);  // a grpSp nests shapes like a spTree
            out.push_back(std::move(shape));
        }
```

(`set_identity` already reads the group's `p:cNvPr` via `nv_cnvpr`, and group geometry via `p:grpSpPr/a:xfrm`. A `p:grpSp`'s child shapes sit directly under it, interleaved with its `p:nvGrpSpPr`/`p:grpSpPr`, so the same dispatch loop handles them — non-shape children are skipped.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide:*"`
Expected: all Slide cases PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): recurse into group shapes preserving reading order (#7)"
```

---

## Task 7: Ordered slide parts in Presentation + load-from-package integration

**Files:**
- Modify: `include/pptctl/presentation.hpp`
- Modify: `src/lib/presentation.cpp`
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_slide.cpp` a `#include <pptctl/presentation.hpp>` (with the other includes), a fixtures helper, and two tests:

```cpp
#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif
```

```cpp
namespace {
std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}
}  // namespace
```

```cpp
TEST_CASE("Presentation: slide_parts returns slide part names in order") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    const auto& parts = pres.slide_parts();
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "ppt/slides/slide1.xml");
    CHECK(parts[1] == "ppt/slides/slide2.xml");
    CHECK(parts[2] == "ppt/slides/slide3.xml");
    CHECK(parts.size() == pres.slide_count());
}

TEST_CASE("Slide: loads slide1 text from three_slides.pptx") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));
    CHECK(slide.part_name() == "ppt/slides/slide1.xml");

    // The first slide's title placeholder reads "Slide 1".
    bool found_title = false;
    for (const auto& s : slide.shapes()) {
        for (const auto& p : s.paragraphs) {
            if (p.text() == "Slide 1") {
                found_title = true;
            }
        }
    }
    CHECK(found_title);
}

TEST_CASE("Slide: images_deck slides carry picture shapes") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    REQUIRE_FALSE(pres.slide_parts().empty());

    bool any_picture = false;
    for (const auto& part : pres.slide_parts()) {
        pptctl::Slide slide;
        REQUIRE(slide.load(pkg, part));
        for (const auto& s : slide.shapes()) {
            if (s.kind == pptctl::ShapeKind::Picture) {
                any_picture = true;
            }
        }
    }
    CHECK(any_picture);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `./build/macos-release/pptctl_tests --test-case="Presentation: slide_parts*"`
Expected: FAIL to compile — `pres.slide_parts()` does not exist yet.

- [ ] **Step 3: Add `slide_parts()` to Presentation**

In `include/pptctl/presentation.hpp`, add a getter next to the other accessors (after `slide_count()`):

```cpp
    [[nodiscard]] const std::vector<std::string>& slide_parts() const noexcept {
        return slide_parts_;
    }
```

And a member next to `slide_count_`:

```cpp
    std::vector<std::string> slide_parts_;
```

- [ ] **Step 4: Populate `slide_parts_` during load**

In `src/lib/presentation.cpp`, replace the `p:sldIdLst` counting loop (the block that increments `slide_count_`) with one that also resolves each slide's relationship target:

```cpp
    auto sld_id_lst = pres.child("p:sldIdLst");
    if (sld_id_lst) {
        for (auto sld_id : sld_id_lst.children("p:sldId")) {
            ++slide_count_;
            const char* rid = sld_id.attribute("r:id").as_string("");
            if (rid[0] != '\0') {
                std::string target = package_->resolve("ppt/presentation.xml", rid);
                if (!target.empty()) {
                    slide_parts_.push_back(std::move(target));
                }
            }
        }
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Presentation: slide_parts*","Slide: loads slide1*","Slide: images_deck*"`
Expected: all three PASS. Also run the full package suite to confirm no regression: `./build/macos-release/pptctl_tests --test-case="Presentation:*"`

- [ ] **Step 6: Commit**

```bash
clang-format -i include/pptctl/presentation.hpp src/lib/presentation.cpp tests/test_slide.cpp
git add include/pptctl/presentation.hpp src/lib/presentation.cpp tests/test_slide.cpp
git commit -m "feat(extract): resolve ordered slide parts and load slides from package (#7)"
```

---

## Task 8: Wire up the `extract` command with a minimal text dump

**Files:**
- Create: `src/cmd/extract.cpp`
- Modify: `src/main.cpp` (forward-declare, register subcommand, drop the `extract` placeholder)
- Modify: `CMakeLists.txt` (add `src/cmd/extract.cpp` to the `pptctl` executable)

- [ ] **Step 1: Write the extract command**

Create `src/cmd/extract.cpp`:

```cpp
// pptctl extract — walks slide shape trees and prints their text structure.
//
// This is a minimal human-readable dump. Structured formats (--format
// json|md|txt) and the versioned schema are tracked separately (#12/#13).

#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>

namespace pptctl::cmd {

namespace {

std::string_view kind_label(ShapeKind kind) {
    switch (kind) {
        case ShapeKind::Placeholder:
            return "placeholder";
        case ShapeKind::TextBox:
            return "textbox";
        case ShapeKind::Picture:
            return "picture";
        case ShapeKind::Table:
            return "table";
        case ShapeKind::Chart:
            return "chart";
        case ShapeKind::Diagram:
            return "diagram";
        case ShapeKind::Ole:
            return "ole";
        case ShapeKind::Group:
            return "group";
        case ShapeKind::Connector:
            return "connector";
        case ShapeKind::Other:
            return "other";
    }
    return "other";
}

void print_shape(const Shape& shape, int depth) {
    const std::string pad(static_cast<std::size_t>(depth) * 2 + 2, ' ');

    if (!shape.paragraphs.empty()) {
        for (const auto& para : shape.paragraphs) {
            const std::string bullet(static_cast<std::size_t>(para.level) * 2, ' ');
            const std::string text = para.text();
            if (shape.placeholder_type && &para == &shape.paragraphs.front()) {
                pptctl::println("{}[{}] {}", pad, *shape.placeholder_type, text);
            } else if (!text.empty()) {
                pptctl::println("{}{}{}", pad, bullet, text);
            }
        }
    } else if (shape.kind == ShapeKind::Group) {
        pptctl::println("{}[group] \"{}\"", pad, shape.name);
    } else {
        pptctl::println("{}[{}] \"{}\"", pad, kind_label(shape.kind), shape.name);
    }

    for (const auto& child : shape.children) {
        print_shape(child, depth + 1);
    }
}

}  // namespace

int extract(const std::string& path) {
    pptctl::Package pkg;
    if (!pkg.open(path)) {
        pptctl::eprintln("error: could not open {}", path);
        return 1;
    }
    pptctl::Presentation pres(pkg);
    if (!pres.load()) {
        pptctl::eprintln("error: {} is not a valid .pptx (no ppt/presentation.xml)", path);
        return 1;
    }

    std::size_t n = 0;
    for (const auto& part : pres.slide_parts()) {
        ++n;
        pptctl::Slide slide;
        if (!slide.load(pkg, part)) {
            pptctl::eprintln("warning: could not read {}", part);
            continue;
        }
        slide.set_index(n);
        pptctl::println("slide {}", slide.index());
        for (const auto& shape : slide.shapes()) {
            print_shape(shape, 0);
        }
    }
    return 0;
}

}  // namespace pptctl::cmd
```

- [ ] **Step 2: Register the command in main.cpp**

In `src/main.cpp`, add the forward declaration next to `info`:

```cpp
int info(const std::string& path, bool json);
int extract(const std::string& path);
```

Remove the `extract` placeholder subcommand (the `app.add_subcommand("extract", …)` block that calls `not_implemented("extract")`) and register a real one in its place (next to the `info` registration):

```cpp
    // --- extract -------------------------------------------------------------
    std::string extract_path;
    auto* extract_cmd =
        app.add_subcommand("extract", "Extract slide text content as a structured dump");
    extract_cmd->add_option("file", extract_path, "path to .pptx")
        ->required()
        ->check(CLI::ExistingFile);
    extract_cmd->callback([&]() { std::exit(pptctl::cmd::extract(extract_path)); });
```

- [ ] **Step 3: Add the source to CMake**

In `CMakeLists.txt`, add `src/cmd/extract.cpp` to the `pptctl` executable (currently lines ~127-130):

```cmake
add_executable(pptctl
    src/main.cpp
    src/cmd/info.cpp
    src/cmd/extract.cpp
)
```

- [ ] **Step 4: Configure, build, and verify manually**

Run: `cmake --preset macos-release && cmake --build --preset macos-release --parallel`
Expected: builds cleanly with no warnings.

Run: `./build/macos-release/pptctl extract tests/fixtures/three_slides.pptx`
Expected: three `slide N` headers; slide 1 shows a line like `[ctrTitle] Slide 1`.

Run: `./build/macos-release/pptctl extract tests/fixtures/images_deck.pptx`
Expected: per-slide output including `[picture] "..."` lines.

Run: `./build/macos-release/pptctl extract tests/real_world/edu_stanford_cs345d_lecture01.pptx | head -40`
Expected: a `group` with indented children appears, text reads in a sensible order, no crash.

- [ ] **Step 5: Run the full test suite**

Run: `ctest --test-dir build/macos-release --output-on-failure`
Expected: the `unit` test passes.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/cmd/extract.cpp src/main.cpp
git add src/cmd/extract.cpp src/main.cpp CMakeLists.txt
git commit -m "feat(extract): wire up the extract command with a minimal text dump (#7)"
```

---

## Task 9: Full verification and ASan pass

**Files:** none (verification only)

- [ ] **Step 1: Clean release build + full test**

Run: `./scripts/build.sh`
Expected: configure + build + `ctest` all green, no warnings from project code.

- [ ] **Step 2: AddressSanitizer build + test**

Run: `./scripts/build.sh asan`
Expected: build + tests pass with no ASan/UBSan reports. (Catches any out-of-bounds in the XML traversal or string handling.)

- [ ] **Step 3: Spot-check extract across several real-world decks**

Run: `for f in tests/real_world/*.pptx; do echo "== $f =="; ./build/macos-release/pptctl extract "$f" >/dev/null || echo "FAILED: $f"; done`
Expected: no `FAILED:` lines and no crashes (exit 0 for every deck).

- [ ] **Step 4: Confirm clang-format is clean**

Run: `clang-format --dry-run --Werror include/pptctl/slide.hpp src/lib/slide.cpp src/cmd/extract.cpp tests/test_slide.cpp src/main.cpp src/lib/presentation.cpp include/pptctl/presentation.hpp`
Expected: no output (all formatted).

- [ ] **Step 5: Final commit (only if step 4 made changes)**

```bash
git add -A
git commit -m "chore(extract): formatting pass (#7)"
```

(If there is nothing to commit, skip — do not create an empty commit.)

---

## Notes for the implementer

- **`reinterpret_cast` in `Slide::load`** converts `std::byte*` to `const char*` for pugixml; this is a permitted C++ cast (not flagged by `-Wold-style-cast`).
- **pugixml does no namespace processing** — node names like `"a:p"` and `"p:sp"` are matched as literal strings, exactly as `presentation.cpp` already does. Test XML namespace declarations are cosmetic.
- **Reading order** is simply pugixml document order: iterating `children()` yields elements as they appear in the file. Preserve that — never sort shapes or runs.
- **Spec reference:** every behavior here maps to `docs/superpowers/specs/2026-05-27-extract-slide-content-parsing-design.md`. The one addition beyond the spec is the `Slide::parse(std::string_view)` test seam, which keeps the parser unit-testable without packaging XML into a `.pptx` (and sidesteps that python-pptx can't synthesize group shapes).
```
