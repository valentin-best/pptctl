// Diff engine — picture-level ops. See diff_internal.hpp.
//
// Pictures are paired by shape id, then by name (groups walked), and each
// pair is compared on media identity, embed/link mode, and alt text. Media
// identity is the content hash a loaded Picture carries; a parse-only Picture
// (no package) falls back to its relationship target, then its rId.

#include <cstddef>
#include <cstdint>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/print.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

// ── matching ─────────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void collect_pictures(const std::vector<Shape>& shapes, std::vector<const Shape*>& out) {
    for (const auto& s : shapes) {
        if (s.kind == ShapeKind::Picture && s.picture) {
            out.push_back(&s);
        }
        if (!s.children.empty()) {
            collect_pictures(s.children, out);
        }
    }
}

struct Match {
    const Shape* a;  // nullptr when Added
    const Shape* b;  // nullptr when Removed
};

// Pairs A's pictures with B's: first by shape id, then by name among the
// leftovers (first come, first served, in document order). Unpaired ones are
// listed on their own side. Result is in A order for pairs and removals, with
// additions appended in B order.
std::vector<Match> match_pictures(const std::vector<const Shape*>& a,
                                  const std::vector<const Shape*>& b) {
    std::vector<bool> b_used(b.size(), false);
    std::vector<const Shape*> a_partner(a.size(), nullptr);

    const auto claim = [&](std::size_t i, auto key) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (!b_used[j] && key(*b[j]) == key(*a[i])) {
                b_used[j] = true;
                a_partner[i] = b[j];
                return;
            }
        }
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        claim(i, [](const Shape& s) -> const std::string& { return s.id; });
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a_partner[i] == nullptr && !a[i]->name.empty()) {
            claim(i, [](const Shape& s) -> const std::string& { return s.name; });
        }
    }

    std::vector<Match> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        out.push_back({a[i], a_partner[i]});
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
        if (!b_used[j]) {
            out.push_back({nullptr, b[j]});
        }
    }
    return out;
}

// ── comparison ───────────────────────────────────────────────────────────────

// What "the same image" means for a picture: its content hash when the media
// resolved; otherwise the part it points at; otherwise the rId itself (all a
// parse-only slide has). Mirrors the slide fingerprint in diff.cpp.
std::string media_identity(const Picture& p) {
    if (p.media_hash) {
        return std::format("{:016x}", *p.media_hash);
    }
    return p.target.empty() ? p.rel_id : p.target;
}

bool alt_text_differs(const Picture& x, const Picture& y) {
    return x.alt_text != y.alt_text || x.title != y.title || x.decorative != y.decorative;
}

// `named` is the shape the op is reported under: B's when it exists there.
PictureChange make_change(PictureOp op,
                          const Shape& named,
                          std::optional<Picture> before,
                          std::optional<Picture> after) {
    PictureChange c;
    c.op = op;
    c.shape_id = named.id;
    c.shape_name = named.name;
    c.before = std::move(before);
    c.after = std::move(after);
    return c;
}

// ── rendering ────────────────────────────────────────────────────────────────

std::string_view basename_of(std::string_view part) {
    const auto slash = part.find_last_of('/');
    return slash == std::string_view::npos ? part : part.substr(slash + 1);
}

// "image1.png 120 B 64x48", "linked rId5", or whatever subset is known.
std::string media_label(const Picture& p) {
    std::string out;
    if (p.linked) {
        out = "linked " + p.rel_id;
    } else if (!p.target.empty()) {
        out = std::string(basename_of(p.target));
    } else {
        out = p.rel_id;
    }
    if (p.size_bytes) {
        out += ' ';
        out += human_bytes(static_cast<std::size_t>(*p.size_bytes));
    }
    if (p.dimensions) {
        out += std::format(" {}x{}", p.dimensions->width, p.dimensions->height);
    }
    return out;
}

// media_label of a side that may be missing (a malformed change); never
// reached for ops built by diff_pictures, which always fills the sides it uses.
std::string side_label(const std::optional<Picture>& side) {
    if (!side) {
        return "?";
    }
    return media_label(*side);
}

// side_label prefixed with the embed/link mode, for Relinked.
std::string mode_label(const std::optional<Picture>& side) {
    if (!side) {
        return "?";
    }
    return (side->linked ? "" : "embedded ") + media_label(*side);
}

std::string quoted_or_none(const std::optional<std::string>& s) {
    return s ? std::format("\"{}\"", *s) : std::string("none");
}

// "alt text \"a\" -> \"b\", title none -> \"t\", decorative no -> yes": only
// the fields that differ.
std::string alt_text_delta(const Picture& x, const Picture& y) {
    std::string out;
    const auto add = [&](std::string_view label, const std::string& from, const std::string& to) {
        if (!out.empty()) {
            out += ", ";
        }
        out += std::format("{} {} -> {}", label, from, to);
    };
    if (x.alt_text != y.alt_text) {
        add("alt text", quoted_or_none(x.alt_text), quoted_or_none(y.alt_text));
    }
    if (x.title != y.title) {
        add("title", quoted_or_none(x.title), quoted_or_none(y.title));
    }
    if (x.decorative != y.decorative) {
        add("decorative", x.decorative ? "yes" : "no", y.decorative ? "yes" : "no");
    }
    return out;
}

nlohmann::json side_to_json(const std::optional<Picture>& side) {
    if (!side) {
        return nullptr;
    }
    const Picture& p = *side;
    nlohmann::json j;
    j["rel_id"] = p.rel_id;
    j["linked"] = p.linked;
    j["target"] = p.target;
    j["content_type"] = p.content_type;
    j["size_bytes"] = p.size_bytes ? nlohmann::json(*p.size_bytes) : nlohmann::json(nullptr);
    j["media_hash"] = p.media_hash ? nlohmann::json(std::format("{:016x}", *p.media_hash))
                                   : nlohmann::json(nullptr);
    j["width"] = p.dimensions ? nlohmann::json(p.dimensions->width) : nlohmann::json(nullptr);
    j["height"] = p.dimensions ? nlohmann::json(p.dimensions->height) : nlohmann::json(nullptr);
    j["alt_text"] = p.alt_text ? nlohmann::json(*p.alt_text) : nlohmann::json(nullptr);
    j["title"] = p.title ? nlohmann::json(*p.title) : nlohmann::json(nullptr);
    j["decorative"] = p.decorative;
    return j;
}

}  // namespace

std::string_view to_string(PictureOp op) noexcept {
    switch (op) {
        case PictureOp::Added:
            return "added";
        case PictureOp::Removed:
            return "removed";
        case PictureOp::Replaced:
            return "replaced";
        case PictureOp::Relinked:
            return "relinked";
        case PictureOp::AltTextChanged:
            return "alt_text_changed";
    }
    return "unknown";
}

std::string describe(const PictureChange& change) {
    std::string head = std::format("picture \"{}\"", change.shape_name);
    switch (change.op) {
        case PictureOp::Added:
            return std::format("{} added ({})", head, side_label(change.after));
        case PictureOp::Removed:
            return std::format("{} removed ({})", head, side_label(change.before));
        case PictureOp::Replaced:
            return std::format("{} replaced ({} -> {})",
                               head,
                               side_label(change.before),
                               side_label(change.after));
        case PictureOp::Relinked:
            return std::format("{} re-linked ({} -> {})",
                               head,
                               mode_label(change.before),
                               mode_label(change.after));
        case PictureOp::AltTextChanged:
            if (change.before && change.after) {
                return std::format("{} alt text changed ({})",
                                   head,
                                   alt_text_delta(*change.before, *change.after));
            }
            return std::format("{} alt text changed", head);
    }
    return head;
}

nlohmann::json to_json(const PictureChange& change) {
    nlohmann::json j;
    j["op"] = to_string(change.op);
    j["shape_id"] = change.shape_id;
    j["shape_name"] = change.shape_name;
    j["before"] = side_to_json(change.before);
    j["after"] = side_to_json(change.after);
    return j;
}

namespace detail {

void diff_pictures(const Slide& a,
                   const Slide& b,
                   const DiffOptions& /*opts*/,
                   std::vector<PictureChange>& out) {
    std::vector<const Shape*> pa;
    std::vector<const Shape*> pb;
    collect_pictures(a.shapes(), pa);
    collect_pictures(b.shapes(), pb);

    for (const auto& m : match_pictures(pa, pb)) {
        if (m.a == nullptr) {
            out.push_back(make_change(PictureOp::Added, *m.b, std::nullopt, m.b->picture));
            continue;
        }
        if (m.b == nullptr) {
            out.push_back(make_change(PictureOp::Removed, *m.a, m.a->picture, std::nullopt));
            continue;
        }
        if (!m.a->picture || !m.b->picture) {
            continue;  // collect_pictures only keeps shapes with a picture
        }
        const Picture& x = *m.a->picture;
        const Picture& y = *m.b->picture;
        if (x.linked != y.linked) {
            out.push_back(make_change(PictureOp::Relinked, *m.b, x, y));
        } else if (media_identity(x) != media_identity(y)) {
            out.push_back(make_change(PictureOp::Replaced, *m.b, x, y));
        }
        if (alt_text_differs(x, y)) {
            out.push_back(make_change(PictureOp::AltTextChanged, *m.b, x, y));
        }
    }
}

}  // namespace detail

}  // namespace pptctl
