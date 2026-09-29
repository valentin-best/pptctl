// pptctl/slide.cpp — parses a slide's shape tree into a content model.

#include <cstddef>
#include <format>
#include <pptctl/image.hpp>
#include <pptctl/slide.hpp>
#include <pugixml.hpp>
#include <string_view>

namespace pptctl {

std::string TableCell::text() const {
    std::string out;
    for (const auto& p : paragraphs) {
        if (!out.empty()) {
            out += '\n';
        }
        out += p.text();
    }
    return out;
}

std::string Slide::notes_text() const {
    std::string out;
    for (const auto& p : notes_) {
        if (!out.empty()) {
            out += '\n';
        }
        out += p.text();
    }
    return out;
}

namespace {

// The shape's identifying p:cNvPr lives inside the shape's first child (the
// nv* container: p:nvSpPr / p:nvPicPr / p:nvGrpSpPr / etc.), regardless of
// shape kind.
pugi::xml_node nv_cnvpr(pugi::xml_node shape_el) {
    return shape_el.first_child().child("p:cNvPr");
}

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
    if (auto ea = rpr.child("a:ea")) {
        if (auto tf = ea.attribute("typeface")) {
            props.font_ea = tf.as_string("");
        }
    }
    if (auto cs = rpr.child("a:cs")) {
        if (auto tf = cs.attribute("typeface")) {
            props.font_cs = tf.as_string("");
        }
    }
    return props;
}

void parse_paragraphs(pugi::xml_node tx_body, std::vector<Paragraph>& out) {
    for (auto p : tx_body.children("a:p")) {
        Paragraph para;
        para.level = p.child("a:pPr").attribute("lvl").as_int(0);
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
        out.push_back(std::move(para));
    }
}

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
    // Groups also declare the coordinate space their children are drawn in.
    auto choff = xfrm.child("a:chOff");
    auto chext = xfrm.child("a:chExt");
    if (choff || chext) {
        g.child_space = ChildSpace{choff.attribute("x").as_llong(0),
                                   choff.attribute("y").as_llong(0),
                                   chext.attribute("cx").as_llong(0),
                                   chext.attribute("cy").as_llong(0)};
    }
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

Table parse_table(pugi::xml_node tbl) {
    Table table;
    std::size_t cols = 0;
    for (auto gc : tbl.child("a:tblGrid").children("a:gridCol")) {
        (void)gc;
        ++cols;
    }
    table.columns = cols;
    for (auto tr : tbl.children("a:tr")) {
        TableRow row;
        for (auto tc : tr.children("a:tc")) {
            TableCell cell;
            cell.col_span = tc.attribute("gridSpan").as_int(1);
            cell.row_span = tc.attribute("rowSpan").as_int(1);
            cell.merged = tc.attribute("hMerge").as_bool() || tc.attribute("vMerge").as_bool();
            parse_paragraphs(tc.child("a:txBody"), cell.paragraphs);
            row.cells.push_back(std::move(cell));
        }
        table.rows.push_back(std::move(row));
    }
    return table;
}

Shape parse_graphic_frame(pugi::xml_node gf_el) {
    Shape shape;
    shape.kind = graphic_frame_kind(gf_el);
    set_identity(gf_el, shape);
    auto gd = gf_el.child("a:graphic").child("a:graphicData");
    if (shape.kind == ShapeKind::Table) {
        if (auto tbl = gd.child("a:tbl")) {
            shape.table = parse_table(tbl);
        }
    } else if (shape.kind == ShapeKind::Chart) {
        if (auto cc = gd.child("c:chart")) {
            Chart chart;
            chart.rel_id = cc.attribute("r:id").as_string("");
            shape.chart = std::move(chart);
        }
    }
    return shape;
}

Shape parse_sp(pugi::xml_node sp_el) {
    Shape shape;
    set_identity(sp_el, shape);
    auto ph = sp_el.child("p:nvSpPr").child("p:nvPr").child("p:ph");
    if (ph) {
        shape.kind = ShapeKind::Placeholder;
        if (auto type = ph.attribute("type")) {
            shape.placeholder_type = type.as_string("");
        }
        if (auto idx = ph.attribute("idx")) {
            shape.placeholder_idx = idx.as_int(0);
        }
    } else {
        shape.kind = ShapeKind::TextBox;
    }
    if (auto tx = sp_el.child("p:txBody")) {
        parse_paragraphs(tx, shape.paragraphs);
    }
    return shape;
}

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
    auto cnvpr = nv_cnvpr(pic_el);
    if (auto descr = cnvpr.attribute("descr")) {
        const std::string_view text = descr.as_string("");
        if (!text.empty()) {
            pic.alt_text = std::string(text);
        }
    }
    if (auto title = cnvpr.attribute("title")) {
        const std::string_view text = title.as_string("");
        if (!text.empty()) {
            pic.title = std::string(text);
        }
    }
    // PowerPoint marks decorative images with an adec:decorative extension
    // inside the cNvPr extLst; element-name match keeps us namespace-prefix
    // agnostic (the prefix is producer-chosen).
    for (auto ext : cnvpr.child("a:extLst").children("a:ext")) {
        for (auto child : ext.children()) {
            const std::string_view name = child.name();
            const auto colon = name.rfind(':');
            const std::string_view local =
                colon == std::string_view::npos ? name : name.substr(colon + 1);
            if (local == "decorative" && child.attribute("val").as_int(0) != 0) {
                pic.decorative = true;
            }
        }
    }
    shape.picture = std::move(pic);
    return shape;
}

// Recurses into p:grpSp to walk nested shape trees; the depth mirrors the
// deck's group nesting, which is shallow in practice.
// NOLINTNEXTLINE(misc-no-recursion)
void parse_sp_tree(pugi::xml_node tree, std::vector<Shape>& out) {
    for (auto el : tree.children()) {
        const std::string_view name = el.name();
        if (name == "p:sp") {
            out.push_back(parse_sp(el));
        } else if (name == "p:pic") {
            out.push_back(parse_pic(el));
        } else if (name == "p:graphicFrame") {
            out.push_back(parse_graphic_frame(el));
        } else if (name == "p:cxnSp") {
            Shape shape;
            shape.kind = ShapeKind::Connector;
            set_identity(el, shape);
            out.push_back(std::move(shape));
        } else if (name == "p:grpSp") {
            Shape shape;
            shape.kind = ShapeKind::Group;
            set_identity(el, shape);
            parse_sp_tree(el, shape.children);  // a grpSp nests shapes like a spTree
            out.push_back(std::move(shape));
        }
    }
}

// Resolves each embedded picture's blip rId to its media part via the package's
// relationship graph, recursing into group children, and identifies the media
// by content: a hash (cached by the package, so shared media costs one read
// per deck) and, for PNG/JPEG, the pixel size from the part's first bytes.
// Linked (external) and unresolved pictures are left as parsed.
// NOLINTNEXTLINE(misc-no-recursion)
void resolve_pictures(const Package& pkg, std::string_view part_name, std::vector<Shape>& shapes) {
    // A PNG's IHDR is in the first 33 bytes; a JPEG's SOF sits behind its APP
    // segments (EXIF, ICC), which are each at most 64 KiB. Anything past that
    // is a pathological header and simply reports no dimensions.
    constexpr std::size_t kHeaderBytes = 65'536;
    for (auto& shape : shapes) {
        if (shape.picture && !shape.picture->linked && !shape.picture->rel_id.empty()) {
            Picture& pic = *shape.picture;
            pic.target = pkg.resolve(part_name, pic.rel_id);
            if (!pic.target.empty()) {
                pic.content_type = pkg.content_type(pic.target);
                pic.size_bytes = pkg.part_size(pic.target);
                pic.media_hash = pkg.part_hash(pic.target);
                pic.dimensions =
                    probe_image_dimensions(pkg.read_part_prefix(pic.target, kHeaderBytes));
            }
        }
        if (!shape.children.empty()) {
            resolve_pictures(pkg, part_name, shape.children);
        }
    }
}

// Extracts text from c:tx — checks both the inline c:v path and the
// c:strRef/c:strCache/c:pt[0]/c:v path that python-pptx writes.
std::string chart_tx_text(pugi::xml_node tx) {
    if (auto v = tx.child("c:v")) {
        return v.text().as_string("");
    }
    return tx.child("c:strRef")
        .child("c:strCache")
        .find_child_by_attribute("c:pt", "idx", "0")
        .child("c:v")
        .text()
        .as_string("");
}

// Hard ceiling for chart point counts — prevents memory exhaustion from a
// maliciously large c:ptCount in an untrusted .pptx.
constexpr std::size_t kMaxChartPoints = 1U << 16;  // 65536 points per series

// Collects string values from c:strLit/c:pt or c:strRef/c:strCache/c:pt.
// Points are placed at their idx positions so sparse sequences stay aligned
// with the matching numeric series. The vector grows lazily as each c:pt is
// encountered (not from c:ptCount) so a malicious ptCount can't exhaust memory.
std::vector<std::string> collect_str_pts(pugi::xml_node parent) {
    pugi::xml_node container;
    if (auto lit = parent.child("c:strLit")) {
        container = lit;
    } else if (auto cache = parent.child("c:strRef").child("c:strCache")) {
        container = cache;
    }
    if (!container) {
        return {};
    }
    std::vector<std::string> out;
    for (auto pt : container.children("c:pt")) {
        const std::size_t idx = pt.attribute("idx").as_uint(0);
        if (idx >= kMaxChartPoints) {
            continue;
        }
        if (idx >= out.size()) {
            out.resize(idx + 1);
        }
        out[idx] = pt.child("c:v").text().as_string("");
    }
    return out;
}

// Collects numeric values from c:numLit/c:pt or c:numRef/c:numCache/c:pt.
// Points are placed at their idx positions so sparse sequences (gaps in chart
// data) stay aligned with category labels from the matching series.
// The vector grows lazily — not from c:ptCount — to cap untrusted allocations.
std::vector<double> collect_num_pts(pugi::xml_node parent) {
    pugi::xml_node container;
    if (auto lit = parent.child("c:numLit")) {
        container = lit;
    } else if (auto cache = parent.child("c:numRef").child("c:numCache")) {
        container = cache;
    }
    if (!container) {
        return {};
    }
    std::vector<double> out;
    for (auto pt : container.children("c:pt")) {
        const std::size_t idx = pt.attribute("idx").as_uint(0);
        if (idx >= kMaxChartPoints) {
            continue;
        }
        if (idx >= out.size()) {
            out.resize(idx + 1, 0.0);
        }
        out[idx] = pt.child("c:v").text().as_double(0.0);
    }
    return out;
}

// Strips the "c:" namespace prefix and "Chart" suffix from an OOXML chart type
// element name, e.g. "c:barChart" → "bar", "c:bar3DChart" → "bar3D".
std::string chart_type_label(std::string_view element_name) {
    // Strip leading "c:"
    if (element_name.starts_with("c:")) {
        element_name.remove_prefix(2);
    }
    // Strip trailing "Chart"
    constexpr std::string_view kSuffix = "Chart";
    if (element_name.ends_with(kSuffix)) {
        element_name.remove_suffix(kSuffix.size());
    }
    return std::string(element_name);
}

void load_chart_data(Chart& chart, std::string_view xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_auto)) {
        return;
    }
    auto cs = doc.child("c:chartSpace");
    auto ch = cs.child("c:chart");

    // Title: c:chart/c:title/c:tx or c:chart/c:title/c:tx/c:rich/a:p/a:r/a:t
    auto title_tx = ch.child("c:title").child("c:tx");
    if (title_tx) {
        // Rich text path (most common)
        std::string rich;
        for (auto p : title_tx.child("c:rich").children("a:p")) {
            for (auto r : p.children("a:r")) {
                rich += r.child("a:t").text().as_string("");
            }
        }
        if (!rich.empty()) {
            chart.title = std::move(rich);
        } else {
            // Plain text path
            auto plain = chart_tx_text(title_tx);
            if (!plain.empty()) {
                chart.title = std::move(plain);
            }
        }
    }

    // Chart types and series from c:plotArea
    auto plot = ch.child("c:plotArea");
    std::string type_str;
    for (auto el : plot.children()) {
        const std::string_view el_name = el.name();
        if (!el_name.starts_with("c:") || !el_name.ends_with("Chart")) {
            continue;
        }
        if (!type_str.empty()) {
            type_str += '+';
        }
        type_str += chart_type_label(el_name);

        for (auto ser : el.children("c:ser")) {
            ChartSeries s;
            s.name = chart_tx_text(ser.child("c:tx"));
            s.categories = collect_str_pts(ser.child("c:cat"));
            // Scatter charts use c:xVal/c:yVal instead of c:cat/c:val.
            // X values are numeric, so convert them to strings for display.
            if (s.categories.empty()) {
                for (const double x : collect_num_pts(ser.child("c:xVal"))) {
                    s.categories.emplace_back(x == static_cast<double>(static_cast<long long>(x))
                                                  ? std::format("{}", static_cast<long long>(x))
                                                  : std::format("{:.6g}", x));
                }
            }
            s.values = collect_num_pts(ser.child("c:val"));
            if (s.values.empty()) {
                s.values = collect_num_pts(ser.child("c:yVal"));
            }
            chart.series.emplace_back(std::move(s));
        }
    }
    chart.chart_type = std::move(type_str);
}

// NOLINTNEXTLINE(misc-no-recursion)
void resolve_charts(const Package& pkg, std::string_view part_name, std::vector<Shape>& shapes) {
    for (auto& shape : shapes) {
        if (shape.chart && !shape.chart->rel_id.empty()) {
            Chart& c = *shape.chart;
            c.target = pkg.resolve(part_name, c.rel_id);
            if (!c.target.empty()) {
                const auto bytes = pkg.read_part(c.target);
                if (!bytes.empty()) {
                    load_chart_data(c,
                                    std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                     bytes.size()));
                }
            }
        }
        if (!shape.children.empty()) {
            resolve_charts(pkg, part_name, shape.children);
        }
    }
}

// The notes text lives in the notesSlide's body placeholder
// (p:sp with p:ph type="body"); the slide-image and slide-number placeholders
// are skipped. Fills out with that placeholder's paragraphs, or leaves it empty
// if there is no body placeholder.
void parse_notes_body(pugi::xml_node tree, std::vector<Paragraph>& out) {
    for (auto sp : tree.children("p:sp")) {
        auto ph = sp.child("p:nvSpPr").child("p:nvPr").child("p:ph");
        if (std::string_view(ph.attribute("type").as_string("")) != "body") {
            continue;
        }
        if (auto tx = sp.child("p:txBody")) {
            parse_paragraphs(tx, out);
        }
        return;  // a notes slide has a single body placeholder
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

bool Slide::parse_notes(std::string_view xml) {
    notes_.clear();
    pugi::xml_document doc;
    const auto result =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        return false;
    }
    auto tree = doc.child("p:notes").child("p:cSld").child("p:spTree");
    if (!tree) {
        return false;
    }
    parse_notes_body(tree, notes_);
    return true;
}

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
    resolve_charts(pkg, part_name_, shapes_);

    // Resolve the slide -> notesSlide relationship and parse its notes body.
    // Notes are optional: a slide with no notesSlide simply has empty notes().
    constexpr std::string_view kNotesRel =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide";
    for (const auto& rel : pkg.relationships(part_name_)) {
        if (rel.type != kNotesRel || rel.external || rel.target.empty()) {
            continue;
        }
        const auto notes_bytes = pkg.read_part(rel.target);
        if (!notes_bytes.empty()) {
            (void)parse_notes(std::string_view(reinterpret_cast<const char*>(notes_bytes.data()),
                                               notes_bytes.size()));
        }
        break;
    }
    return true;
}

}  // namespace pptctl
