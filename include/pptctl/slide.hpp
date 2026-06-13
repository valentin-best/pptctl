// pptctl/slide.hpp
//
// Document layer: the content of a single slide — its shape tree, text
// frames, paragraphs, and runs. Sits on top of the package layer, alongside
// presentation.hpp. Read-only: this layer never serializes slides back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>  // for Emu
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

enum class ShapeKind : std::uint8_t {
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

// One cell of a table (a:tc). Reuses the text model for content. A cell that
// originates a merge carries col_span/row_span > 1; a cell covered by a
// neighbor's span is flagged merged and kept in the grid so row/column indices
// stay true.
struct TableCell {
    std::vector<Paragraph> paragraphs;  // a:tc/a:txBody
    int col_span = 1;                   // a:tc@gridSpan
    int row_span = 1;                   // a:tc@rowSpan
    bool merged = false;                // a:tc@hMerge or @vMerge (covered cell)

    [[nodiscard]] std::string text() const;  // paragraph texts joined with '\n'
};

// One row of a table (a:tr).
struct TableRow {
    std::vector<TableCell> cells;  // a:tc, in document order
};

// A graphic-frame table (a:tbl). Populated only on shapes with kind == Table.
struct Table {
    std::size_t columns = 0;     // a:tblGrid/a:gridCol count
    std::vector<TableRow> rows;  // a:tr, in document order
};

// Present only when the shape carries an a:xfrm. EMU, the native OOXML unit.
struct Geometry {
    Emu off_x = 0;
    Emu off_y = 0;
    Emu ext_cx = 0;
    Emu ext_cy = 0;
};

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
    std::optional<std::string> title;         // p:cNvPr@title
    bool decorative = false;                  // adec:decorative marker in cNvPr extLst
};

// One data series from a chart part. parse() captures rel_id; Slide::load()
// fills the rest from the chart part XML.
struct ChartSeries {
    std::string name;                     // c:ser/c:tx — series label
    std::vector<std::string> categories;  // c:ser/c:cat — category labels (may be empty)
    std::vector<double> values;           // c:ser/c:val — numeric values
};

// Chart metadata from a graphic-frame shape. Present only when kind == Chart.
// parse() fills rel_id from the slide XML c:chart@r:id; Slide::load() resolves
// the chart part and fills chart_type/title/series.
struct Chart {
    std::string rel_id;                // c:chart@r:id in slide XML
    std::string target;                // resolved chart part (ppt/charts/chartN.xml)
    std::string chart_type;            // e.g. "bar", "line", "bar+line" for combo
    std::optional<std::string> title;  // c:chart/c:title text; nullopt if absent
    std::vector<ChartSeries> series;
};

struct Shape {
    ShapeKind kind = ShapeKind::Other;
    std::string id;                               // p:cNvPr@id
    std::string name;                             // p:cNvPr@name
    std::optional<std::string> placeholder_type;  // p:nvPr/p:ph@type
    std::optional<Geometry> geometry;
    std::vector<Paragraph> paragraphs;  // text body; empty for non-text shapes
    std::vector<Shape> children;        // populated only for Group, recursive
    std::optional<Picture> picture;     // populated only when kind == Picture
    std::optional<Table> table;         // populated only when kind == Table
    std::optional<Chart> chart;         // populated only when kind == Chart
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

    // Parse a notesSlide part's XML and capture its notes-body text into
    // notes(). Exposed for testing without a package, mirroring parse().
    // Returns false if the XML can't be parsed or carries no notes spTree;
    // a well-formed notes slide with no body placeholder yields empty notes.
    [[nodiscard]] bool parse_notes(std::string_view xml);

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }
    [[nodiscard]] const std::string& part_name() const noexcept {
        return part_name_;
    }
    [[nodiscard]] const std::vector<Shape>& shapes() const noexcept {
        return shapes_;
    }

    // Speaker-notes paragraphs from the slide's notesSlide, in document order.
    // Empty when the slide has no notes (or none have been loaded yet).
    [[nodiscard]] const std::vector<Paragraph>& notes() const noexcept {
        return notes_;
    }

    // Notes paragraph texts joined with '\n'. Empty when there are no notes.
    [[nodiscard]] std::string notes_text() const;

    void set_index(std::size_t i) noexcept {
        index_ = i;
    }

 private:
    std::size_t index_ = 0;
    std::string part_name_;
    std::vector<Shape> shapes_;
    std::vector<Paragraph> notes_;
};

}  // namespace pptctl
