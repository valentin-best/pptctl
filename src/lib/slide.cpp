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
    if (auto descr = nv_cnvpr(pic_el).attribute("descr")) {
        const std::string_view text = descr.as_string("");
        if (!text.empty()) {
            pic.alt_text = std::string(text);
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
            Shape shape;
            shape.kind = graphic_frame_kind(el);
            set_identity(el, shape);
            out.push_back(std::move(shape));
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
    if (!parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
        return false;
    }
    resolve_pictures(pkg, part_name_, shapes_);
    return true;
}

}  // namespace pptctl
