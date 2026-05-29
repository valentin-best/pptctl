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
