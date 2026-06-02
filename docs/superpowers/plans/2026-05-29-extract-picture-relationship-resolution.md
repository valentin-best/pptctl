# Picture Extraction with Relationship Resolution — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve each `p:pic` in a slide's shape tree to its media part via the per-part relationship graph, and carry image metadata (target part, content type, byte size, alt text) on the shape model and in `extract` output.

**Architecture:** Two-phase. `Slide::parse(xml)` stays package-free and captures the raw blip relationship id (`r:embed` or `r:link`) plus alt text. `Slide::load(pkg, part)` then resolves embedded pictures to their media parts using the package's relationship graph. A new `Package::part_size` reads the uncompressed media size from ZIP metadata (no decompression).

**Tech Stack:** C++20, pugixml (XML), minizip-ng (ZIP), doctest (tests), CMake + Ninja.

**Spec:** `docs/superpowers/specs/2026-05-29-extract-picture-relationship-resolution-design.md`

**Conventions reminders (from CLAUDE.md):**
- Build: `cmake --build --preset macos-release --parallel` (configure once with `cmake --preset macos-release` if `build/macos-release/` is missing).
- Run one test case: `./build/macos-release/pptctl_tests --test-case="<name>"`.
- Run clang-format on touched C++ before every commit; the pre-commit hook runs clang-format + clang-tidy as errors.
- `Emu` is `std::int64_t`; store EMU internally. Set `RunProperties`/optional fields only when the source attribute is present.
- No CMake changes are needed: `src/lib/slide.cpp`, `src/cmd/extract.cpp`, `tests/test_slide.cpp`, `tests/test_package.cpp` are all already in the target lists.

---

### Task 1: `Package::part_size` — uncompressed size from ZIP metadata

**Files:**
- Modify: `include/pptctl/package.hpp` (add include + method declaration)
- Modify: `src/lib/package.cpp` (implement)
- Test: `tests/test_package.cpp`

- [ ] **Step 1: Write the failing test**

Add at the end of `tests/test_package.cpp` (before the final newline, after the last `TEST_CASE`). `images_deck.pptx`'s `ppt/media/image1.png` is exactly 120 bytes.

```cpp
TEST_CASE("Package: part_size returns the uncompressed size of a part") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));

    const auto size = pkg.part_size("ppt/media/image1.png");
    REQUIRE(size.has_value());
    CHECK(*size == 120);
}

TEST_CASE("Package: part_size is nullopt for a missing part") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    CHECK_FALSE(pkg.part_size("does/not/exist.png").has_value());
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cmake --build --preset macos-release --parallel`
Expected: compile error — `part_size` is not a member of `pptctl::Package`.

- [ ] **Step 3: Declare `part_size` in the header**

In `include/pptctl/package.hpp`, add `#include <cstdint>` and `#include <optional>` alongside the existing includes (after `#include <cstddef>`):

```cpp
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
```

Then add this declaration right after the `resolve(...)` declaration (after line 64), inside the public section:

```cpp
    // Uncompressed size of a part, read from the ZIP entry metadata without
    // decompressing it. Returns nullopt if the part doesn't exist or no
    // package is open.
    [[nodiscard]] std::optional<std::uint64_t> part_size(std::string_view name) const;
```

- [ ] **Step 4: Implement `part_size` in the source**

In `src/lib/package.cpp`, add this definition after `Package::resolve(...)` (after the closing brace of `resolve`, before the final `}  // namespace pptctl`):

```cpp
std::optional<std::uint64_t> Package::part_size(std::string_view name) const {
    if (impl_->reader == nullptr) {
        return std::nullopt;
    }
    const std::string name_str(name);
    if (mz_zip_reader_locate_entry(impl_->reader, name_str.c_str(), 0) != MZ_OK) {
        return std::nullopt;
    }
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(impl_->reader, &info) != MZ_OK || info == nullptr) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info->uncompressed_size);
}
```

Add `#include <cstdint>` and `#include <optional>` to the include block in `src/lib/package.cpp` (alongside `#include <cstddef>`).

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Package: part_size*"`
Expected: PASS (2 test cases, all assertions succeed).

- [ ] **Step 6: clang-format and commit**

```bash
clang-format -i include/pptctl/package.hpp src/lib/package.cpp tests/test_package.cpp
git add include/pptctl/package.hpp src/lib/package.cpp tests/test_package.cpp
git commit -m "feat(package): add part_size from ZIP metadata (#8)"
```

---

### Task 2: `Picture` model + `parse()` captures blip references

**Files:**
- Modify: `include/pptctl/slide.hpp` (add `Picture` struct + `Shape::picture` field)
- Modify: `src/lib/slide.cpp` (add `parse_pic`, use it in `parse_sp_tree`)
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing tests**

Add two inline-XML constants inside the anonymous `namespace { ... }` block in `tests/test_slide.cpp` (e.g. after `kMixedShapes`, before `kGroupedShapes`):

```cpp
// A picture whose blip is embedded (internal), with alt text on cNvPr@descr.
constexpr const char* kEmbeddedPicture = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="2" name="Picture 2" descr="A red square"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:embed="rId2"/></p:blipFill>
      <p:spPr/>
    </p:pic>
  </p:spTree></p:cSld>
</p:sld>)";

// A picture whose blip is a link to an external image.
constexpr const char* kLinkedPicture = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="2" name="Linked"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:link="rId5"/></p:blipFill>
      <p:spPr/>
    </p:pic>
  </p:spTree></p:cSld>
</p:sld>)";
```

Add two test cases at the end of `tests/test_slide.cpp`:

```cpp
TEST_CASE("Slide: captures an embedded picture's blip reference and alt text") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kEmbeddedPicture));
    REQUIRE(slide.shapes().size() == 1);

    const auto& pic = slide.shapes()[0];
    CHECK(pic.kind == pptctl::ShapeKind::Picture);
    REQUIRE(pic.picture.has_value());
    CHECK(pic.picture->rel_id == "rId2");
    CHECK_FALSE(pic.picture->linked);
    REQUIRE(pic.picture->alt_text.has_value());
    CHECK(*pic.picture->alt_text == "A red square");
    // parse() does not resolve — that is load()'s job.
    CHECK(pic.picture->target.empty());
    CHECK_FALSE(pic.picture->size_bytes.has_value());
}

TEST_CASE("Slide: flags a linked (external) picture") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kLinkedPicture));
    REQUIRE(slide.shapes().size() == 1);

    const auto& pic = slide.shapes()[0];
    REQUIRE(pic.picture.has_value());
    CHECK(pic.picture->linked);
    CHECK(pic.picture->rel_id == "rId5");
    CHECK_FALSE(pic.picture->alt_text.has_value());
}
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

Run: `cmake --build --preset macos-release --parallel`
Expected: compile error — `Shape` has no member `picture` / no type `Picture`.

- [ ] **Step 3: Add the `Picture` struct and `Shape::picture` field**

In `include/pptctl/slide.hpp`, add the `Picture` struct immediately before `struct Shape` (after the `Geometry` struct):

```cpp
// A picture's blip reference and the media part it resolves to. Present only on
// shapes with kind == Picture. parse() fills rel_id/linked/alt_text from the
// slide XML; Slide::load() fills target/content_type/size_bytes via the
// package's relationship graph (left empty/nullopt for linked or unresolved
// images).
struct Picture {
    std::string rel_id;                       // a:blip@r:embed, or @r:link when linked
    bool linked = false;                      // from r:link (external) rather than r:embed
    std::string target;                       // resolved media part; empty if linked/unresolved
    std::string content_type;                 // e.g. "image/png"; empty if unknown
    std::optional<std::uint64_t> size_bytes;  // uncompressed media size; nullopt if unknown
    std::optional<std::string> alt_text;      // p:cNvPr@descr
};
```

Then add one field to `struct Shape`, after the `children` member:

```cpp
    std::optional<Picture> picture;     // populated only when kind == Picture
```

(`slide.hpp` already includes `<cstdint>`, `<optional>`, `<string>`, and `<vector>` — no new includes needed.)

- [ ] **Step 4: Implement `parse_pic` and call it from `parse_sp_tree`**

In `src/lib/slide.cpp`, add this function in the anonymous namespace, just before `parse_sp_tree` (after `parse_sp`):

```cpp
Shape parse_pic(pugi::xml_node pic_el) {
    Shape shape;
    shape.kind = ShapeKind::Picture;
    set_identity(pic_el, shape);

    Picture pic;
    auto blip = pic_el.child("p:blipFill").child("a:blip");
    if (auto embed = blip.attribute("r:embed")) {
        pic.rel_id = embed.as_string("");
    } else if (auto link = blip.attribute("r:link")) {
        pic.rel_id = link.as_string("");
        pic.linked = true;
    }
    if (auto descr = nv_cnvpr(pic_el).attribute("descr")) {
        const std::string_view text = descr.as_string("");
        if (!text.empty()) {
            pic.alt_text = std::string(text);
        }
    }
    shape.picture = std::move(pic);
    return shape;
}
```

Replace the existing `p:pic` branch in `parse_sp_tree` (currently builds a shallow Picture shape) with:

```cpp
        } else if (name == "p:pic") {
            out.push_back(parse_pic(el));
        } else if (name == "p:graphicFrame") {
```

(Only the `p:pic` branch body changes; leave the surrounding branches intact.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide: captures an embedded picture*" --test-case="Slide: flags a linked*"`
Expected: PASS (2 cases). Also re-run the existing kind-detection case to confirm no regression: `./build/macos-release/pptctl_tests --test-case="Slide: detects shape kinds*"` → PASS.

- [ ] **Step 6: clang-format and commit**

```bash
clang-format -i include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp
git add include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): capture picture blip references in the slide model (#8)"
```

---

### Task 3: `Slide::load` resolves embedded pictures to media parts

**Files:**
- Modify: `src/lib/slide.cpp` (add `resolve_pictures` helper, call it from `Slide::load`)
- Test: `tests/test_slide.cpp`

- [ ] **Step 1: Write the failing test**

Add at the end of `tests/test_slide.cpp`. `images_deck.pptx` slide 1 embeds `rId2 → ppt/media/image1.png` (120-byte PNG), per the fixture's rels.

```cpp
TEST_CASE("Slide: resolves an embedded picture to its media part on load") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));

    const pptctl::Picture* pic = nullptr;
    for (const auto& s : slide.shapes()) {
        if (s.kind == pptctl::ShapeKind::Picture && s.picture.has_value()) {
            pic = &*s.picture;
        }
    }
    REQUIRE(pic != nullptr);
    CHECK(pic->target == "ppt/media/image1.png");
    CHECK(pic->content_type == "image/png");
    REQUIRE(pic->size_bytes.has_value());
    CHECK(*pic->size_bytes == 120);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide: resolves an embedded picture*"`
Expected: FAIL — `pic->target` is empty (resolution not implemented yet).

- [ ] **Step 3: Add the `resolve_pictures` helper**

In `src/lib/slide.cpp`, add this function in the anonymous namespace, just before the namespace's closing `}  // namespace` (after `parse_sp_tree`):

```cpp
// Resolves each embedded picture's blip rId to its media part via the package's
// relationship graph, recursing into group children. Linked (external) and
// unresolved pictures are left as parsed.
// NOLINTNEXTLINE(misc-no-recursion)
void resolve_pictures(const Package& pkg, std::string_view part_name, std::vector<Shape>& shapes) {
    for (auto& shape : shapes) {
        if (shape.picture && !shape.picture->linked && !shape.picture->rel_id.empty()) {
            Picture& pic = *shape.picture;
            pic.target = pkg.resolve(part_name, pic.rel_id);
            if (!pic.target.empty()) {
                pic.content_type = pkg.content_type(pic.target);
                pic.size_bytes = pkg.part_size(pic.target);
            }
        }
        if (!shape.children.empty()) {
            resolve_pictures(pkg, part_name, shape.children);
        }
    }
}
```

- [ ] **Step 4: Call `resolve_pictures` from `Slide::load`**

In `src/lib/slide.cpp`, replace the body of `Slide::load` with:

```cpp
bool Slide::load(const Package& pkg, std::string_view part_name) {
    part_name_ = std::string(part_name);
    const auto bytes = pkg.read_part(part_name);
    if (bytes.empty()) {
        return false;
    }
    if (!parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
        return false;
    }
    resolve_pictures(pkg, part_name_, shapes_);
    return true;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build --preset macos-release --parallel && ./build/macos-release/pptctl_tests --test-case="Slide: resolves an embedded picture*"`
Expected: PASS.

- [ ] **Step 6: Run the whole suite to confirm no regressions**

Run: `ctest --test-dir build/macos-release --output-on-failure`
Expected: `unit` test passes.

- [ ] **Step 7: clang-format and commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): resolve embedded pictures to media parts on load (#8)"
```

---

### Task 4: Show resolved picture metadata in `extract` output

**Files:**
- Modify: `src/cmd/extract.cpp` (add `human_size` + `describe_picture` helpers, branch in `print_shape`)

No unit test: the text dump is an unstable demo surface (formats land in #12), consistent with how #7 shipped the dump without an output golden test. Verified by running the binary against `images_deck.pptx`.

- [ ] **Step 1: Add the helper functions**

In `src/cmd/extract.cpp`, add `#include <array>`, `#include <cstdint>`, and `#include <format>` to the include block. Then add these two functions in the anonymous namespace, before `print_shape`:

```cpp
std::string human_size(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    constexpr std::array<std::string_view, 3> kUnits{"KB", "MB", "GB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

// The "→ ..." suffix describing where a picture resolves to, plus alt text.
std::string describe_picture(const Picture& pic) {
    std::string out;
    if (pic.linked) {
        out = "→ linked (external)";
    } else if (pic.target.empty()) {
        out = std::format("→ unresolved ({})", pic.rel_id);
    } else {
        out = std::format("→ {}", pic.target);
        std::string detail = pic.content_type;
        if (pic.size_bytes) {
            if (!detail.empty()) {
                detail += ", ";
            }
            detail += human_size(*pic.size_bytes);
        }
        if (!detail.empty()) {
            out += std::format(" ({})", detail);
        }
    }
    if (pic.alt_text) {
        out += std::format(" — {}", *pic.alt_text);
    }
    return out;
}
```

- [ ] **Step 2: Add a picture branch in `print_shape`**

In `src/cmd/extract.cpp`, in `print_shape`, add a `Picture` branch between the `Group` branch and the generic `else`:

```cpp
    } else if (shape.kind == ShapeKind::Group) {
        pptctl::println("{}[group] \"{}\"", pad, shape.name);
    } else if (shape.picture) {
        pptctl::println("{}[picture] \"{}\" {}", pad, shape.name, describe_picture(*shape.picture));
    } else {
        pptctl::println("{}[{}] \"{}\"", pad, kind_label(shape.kind), shape.name);
    }
```

- [ ] **Step 3: Build and run against the image fixture**

Run:
```bash
cmake --build --preset macos-release --parallel
./build/macos-release/pptctl extract tests/fixtures/images_deck.pptx | head -6
```
Expected: each picture line shows its resolved part, e.g.:
```
slide 1
  [picture] "Picture 2" → ppt/media/image1.png (image/png, 120 B) — image.png
```
(`images_deck.pptx` sets `descr="image.png"`, so the alt-text suffix appears.)

- [ ] **Step 4: Confirm the full suite still passes**

Run: `ctest --test-dir build/macos-release --output-on-failure`
Expected: `unit` passes.

- [ ] **Step 5: clang-format and commit**

```bash
clang-format -i src/cmd/extract.cpp
git add src/cmd/extract.cpp
git commit -m "feat(extract): show resolved picture metadata in the dump (#8)"
```

---

### Wrap-up

- [ ] **Full clean build + test (host preset):** `./scripts/build.sh` → configures, builds, and runs tests; expect all green.
- [ ] **Open the PR** referencing #8 (per repo workflow: feature branch → PR, never commit to main). Summarize the four commits and note byte-export remains deferred to #12.
```
