// Diff engine — text-level ops. See diff_internal.hpp.
//
// Shapes are matched across the pair (id, then name, then placeholder
// type+idx, groups flattened), each matched pair's paragraphs are aligned by
// LCS over their compared text, and aligned paragraphs are checked for
// formatting differences byte by byte so a re-split run is not a change.

#include <algorithm>
#include <cstddef>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

// ── shape matching ────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void flatten(const std::vector<Shape>& shapes, std::vector<const Shape*>& out) {
    for (const auto& s : shapes) {
        out.push_back(&s);
        flatten(s.children, out);
    }
}

std::string placeholder_key(const Shape& s) {
    if (!s.placeholder_type && !s.placeholder_idx) {
        return {};
    }
    return s.placeholder_type.value_or("") + "|" +
           (s.placeholder_idx ? std::to_string(*s.placeholder_idx) : "");
}

struct ShapePair {
    const Shape* a;
    const Shape* b;
};

// Pair shapes of A with shapes of B, in B order. Three passes of decreasing
// confidence; a shape claimed by an earlier pass is skipped by later ones.
std::vector<ShapePair> match_shapes(const Slide& a, const Slide& b) {
    std::vector<const Shape*> fa;
    std::vector<const Shape*> fb;
    flatten(a.shapes(), fa);
    flatten(b.shapes(), fb);

    std::vector<const Shape*> partner(fb.size(), nullptr);
    std::vector<bool> a_used(fa.size(), false);

    const auto pass = [&](auto&& key_of) {
        std::unordered_map<std::string, std::vector<std::size_t>> by_key;
        for (std::size_t i = fa.size(); i-- > 0;) {
            if (a_used[i]) {
                continue;
            }
            const std::string k = key_of(*fa[i]);
            if (!k.empty()) {
                by_key[k].push_back(i);  // reversed so back() is the first in A order
            }
        }
        for (std::size_t j = 0; j < fb.size(); ++j) {
            if (partner[j] != nullptr) {
                continue;
            }
            const std::string k = key_of(*fb[j]);
            if (k.empty()) {
                continue;
            }
            auto it = by_key.find(k);
            if (it == by_key.end() || it->second.empty()) {
                continue;
            }
            const std::size_t i = it->second.back();
            it->second.pop_back();
            a_used[i] = true;
            partner[j] = fa[i];
        }
    };
    pass([](const Shape& s) { return s.id; });
    pass([](const Shape& s) { return s.name; });
    pass(placeholder_key);

    std::vector<ShapePair> pairs;
    for (std::size_t j = 0; j < fb.size(); ++j) {
        if (partner[j] != nullptr) {
            pairs.push_back({partner[j], fb[j]});
        }
    }
    return pairs;
}

// ── paragraph normalization ───────────────────────────────────────────────────

bool is_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

// A paragraph reduced to what the diff compares: its text — with field runs
// dropped and whitespace collapsed unless strict — plus the run properties
// behind every byte of it, so formatting compares independently of how the
// text happens to be split into runs.
struct Para {
    std::size_t index = 0;  // 1-based in its body
    std::string text;
    std::vector<const RunProperties*> props;  // one per byte of `text`
};

std::vector<Para> normalize(const std::vector<Paragraph>& paras, bool strict) {
    std::vector<Para> out;
    for (std::size_t i = 0; i < paras.size(); ++i) {
        Para p;
        p.index = i + 1;
        bool pending_space = false;
        for (const auto& r : paras[i].runs) {
            if (r.is_field && !strict) {
                continue;
            }
            for (const char ch : r.text) {
                if (!strict && is_space(ch)) {
                    pending_space = true;
                    continue;
                }
                if (pending_space && !p.text.empty()) {
                    p.text.push_back(' ');
                    p.props.push_back(&r.props);
                }
                pending_space = false;
                p.text.push_back(ch);
                p.props.push_back(&r.props);
            }
        }
        if (strict || !p.text.empty()) {
            out.push_back(std::move(p));
        }
    }
    return out;
}

// ── formatting ────────────────────────────────────────────────────────────────

bool same_props(const RunProperties& x, const RunProperties& y) {
    return x.bold == y.bold && x.italic == y.italic && x.underlined == y.underlined &&
           x.size_centipt == y.size_centipt && x.font == y.font && x.font_ea == y.font_ea &&
           x.font_cs == y.font_cs;
}

void describe_flag(std::string_view name,
                   const std::optional<bool>& from,
                   const std::optional<bool>& to,
                   std::vector<std::string>& out) {
    if (from == to) {
        return;
    }
    std::string_view state = "inherit";
    if (to) {
        state = *to ? "on" : "off";
    }
    out.push_back(std::format("{} {}", name, state));
}

void describe_font(std::string_view name,
                   const std::optional<std::string>& from,
                   const std::optional<std::string>& to,
                   std::vector<std::string>& out) {
    if (from == to) {
        return;
    }
    const auto show = [](const std::optional<std::string>& f) {
        return f ? std::format("\"{}\"", *f) : std::string{"inherit"};
    };
    out.push_back(std::format("{} {} -> {}", name, show(from), show(to)));
}

std::string describe_delta(const RunProperties& from, const RunProperties& to) {
    std::vector<std::string> parts;
    describe_flag("bold", from.bold, to.bold, parts);
    describe_flag("italic", from.italic, to.italic, parts);
    describe_flag("underline", from.underlined, to.underlined, parts);
    if (from.size_centipt != to.size_centipt) {
        const auto show = [](const RunProperties& p) {
            return p.size_centipt ? std::format("{:g}pt", p.size_pt()) : std::string{"inherit"};
        };
        parts.push_back(std::format("size {} -> {}", show(from), show(to)));
    }
    describe_font("font", from.font, to.font, parts);
    describe_font("east-asian font", from.font_ea, to.font_ea, parts);
    describe_font("complex-script font", from.font_cs, to.font_cs, parts);
    std::string s;
    for (const auto& part : parts) {
        if (!s.empty()) {
            s += ", ";
        }
        s += part;
    }
    return s;
}

// Spans of `a`/`b` (same text) whose run properties differ, as
// `"span": delta; "span": delta`. Empty when formatting is identical. Unless
// strict, whitespace bytes never open a span and are trimmed from its end.
std::string formatting_diff(const Para& a, const Para& b, bool strict) {
    const std::size_t n = a.text.size();
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        if ((!strict && is_space(a.text[i])) || same_props(*a.props[i], *b.props[i])) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t end = i + 1;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!strict && is_space(a.text[j])) {
                continue;
            }
            if (!same_props(*a.props[j], *a.props[start]) ||
                !same_props(*b.props[j], *b.props[start])) {
                break;
            }
            end = j + 1;
        }
        if (!out.empty()) {
            out += "; ";
        }
        out += std::format("\"{}\": {}",
                           std::string_view{a.text}.substr(start, end - start),
                           describe_delta(*a.props[start], *b.props[start]));
        i = end;
    }
    return out;
}

// ── paragraph alignment ───────────────────────────────────────────────────────

// Indices of LCS-aligned pairs over the compared texts, in order.
std::vector<std::pair<std::size_t, std::size_t>> align(const std::vector<Para>& a,
                                                       const std::vector<Para>& b) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    // lcs[i][j] = LCS length of a[i..] and b[j..]
    std::vector<std::vector<std::size_t>> lcs(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i].text == b[j].text ? lcs[i + 1][j + 1] + 1
                                               : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i].text == b[j].text) {
            pairs.emplace_back(i, j);
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
    return pairs;
}

struct Owner {
    std::string shape;
    std::string shape_id;
    bool notes = false;
};

TextChange make_change(const Owner& owner, TextChangeKind kind) {
    TextChange c;
    c.kind = kind;
    c.shape = owner.shape;
    c.shape_id = owner.shape_id;
    c.notes = owner.notes;
    return c;
}

void diff_paragraphs(const Owner& owner,
                     const std::vector<Paragraph>& a_paras,
                     const std::vector<Paragraph>& b_paras,
                     bool strict,
                     std::vector<TextChange>& out) {
    const auto a = normalize(a_paras, strict);
    const auto b = normalize(b_paras, strict);
    auto pairs = align(a, b);
    pairs.emplace_back(a.size(), b.size());  // sentinel: flush the trailing gap

    // Deleted and inserted paragraphs in the same gap are paired up as changes,
    // as a text diff shows a hunk; the leftovers stand alone.
    const auto flush_gap =
        [&](std::size_t a_from, std::size_t a_to, std::size_t b_from, std::size_t b_to) {
            const std::size_t deleted = a_to - a_from;
            const std::size_t inserted = b_to - b_from;
            const std::size_t changed = std::min(deleted, inserted);
            for (std::size_t k = 0; k < changed; ++k) {
                auto c = make_change(owner, TextChangeKind::Changed);
                c.a_paragraph = a[a_from + k].index;
                c.b_paragraph = b[b_from + k].index;
                c.old_text = a_paras[c.a_paragraph - 1].text();
                c.new_text = b_paras[c.b_paragraph - 1].text();
                out.push_back(std::move(c));
            }
            for (std::size_t k = changed; k < deleted; ++k) {
                auto c = make_change(owner, TextChangeKind::Deleted);
                c.a_paragraph = a[a_from + k].index;
                c.old_text = a_paras[c.a_paragraph - 1].text();
                out.push_back(std::move(c));
            }
            for (std::size_t k = changed; k < inserted; ++k) {
                auto c = make_change(owner, TextChangeKind::Inserted);
                c.b_paragraph = b[b_from + k].index;
                c.new_text = b_paras[c.b_paragraph - 1].text();
                out.push_back(std::move(c));
            }
        };

    std::size_t ai = 0;
    std::size_t bi = 0;
    for (const auto& [pa, pb] : pairs) {
        flush_gap(ai, pa, bi, pb);
        if (pa < a.size()) {
            if (auto fmt = formatting_diff(a[pa], b[pb], strict); !fmt.empty()) {
                auto c = make_change(owner, TextChangeKind::Reformatted);
                c.a_paragraph = a[pa].index;
                c.b_paragraph = b[pb].index;
                c.old_text = a_paras[c.a_paragraph - 1].text();
                c.new_text = b_paras[c.b_paragraph - 1].text();
                c.formatting = std::move(fmt);
                out.push_back(std::move(c));
            }
        }
        ai = pa + 1;
        bi = pb + 1;
    }
}

std::string_view kind_name(TextChangeKind k) noexcept {
    switch (k) {
        case TextChangeKind::Inserted:
            return "inserted";
        case TextChangeKind::Deleted:
            return "deleted";
        case TextChangeKind::Changed:
            return "changed";
        case TextChangeKind::Reformatted:
            return "reformatted";
    }
    return "changed";
}

}  // namespace

namespace detail {

void diff_text(const Slide& a,
               const Slide& b,
               const DiffOptions& opts,
               std::vector<TextChange>& out) {
    for (const auto& [sa, sb] : match_shapes(a, b)) {
        if (sa->paragraphs.empty() && sb->paragraphs.empty()) {
            continue;
        }
        diff_paragraphs(
            {sb->name, sb->id, false}, sa->paragraphs, sb->paragraphs, opts.strict_text, out);
    }
    diff_paragraphs({"", "", true}, a.notes(), b.notes(), opts.strict_text, out);
}

}  // namespace detail

std::string describe(const TextChange& c) {
    const std::string where = c.notes ? "notes" : std::format("shape \"{}\"", c.shape);
    const std::string now =
        c.a_paragraph != 0 && c.b_paragraph != 0 && c.a_paragraph != c.b_paragraph
            ? std::format(" (now {})", c.b_paragraph)
            : std::string{};
    switch (c.kind) {
        case TextChangeKind::Inserted:
            return std::format(
                "{}: paragraph {} inserted: \"{}\"", where, c.b_paragraph, c.new_text);
        case TextChangeKind::Deleted:
            return std::format(
                "{}: paragraph {} deleted: \"{}\"", where, c.a_paragraph, c.old_text);
        case TextChangeKind::Changed:
            return std::format(R"({}: paragraph {}{} changed: "{}" -> "{}")",
                               where,
                               c.a_paragraph,
                               now,
                               c.old_text,
                               c.new_text);
        case TextChangeKind::Reformatted:
            return std::format(
                "{}: paragraph {}{} reformatted: {}", where, c.a_paragraph, now, c.formatting);
    }
    return {};
}

nlohmann::json to_json(const TextChange& c) {
    nlohmann::json j;
    j["kind"] = kind_name(c.kind);
    j["shape"] = c.shape;
    j["shape_id"] = c.shape_id;
    j["notes"] = c.notes;
    j["a_paragraph"] = c.a_paragraph != 0 ? nlohmann::json(c.a_paragraph) : nlohmann::json();
    j["b_paragraph"] = c.b_paragraph != 0 ? nlohmann::json(c.b_paragraph) : nlohmann::json();
    j["old_text"] =
        c.kind != TextChangeKind::Inserted ? nlohmann::json(c.old_text) : nlohmann::json();
    j["new_text"] =
        c.kind != TextChangeKind::Deleted ? nlohmann::json(c.new_text) : nlohmann::json();
    j["formatting"] =
        c.kind == TextChangeKind::Reformatted ? nlohmann::json(c.formatting) : nlohmann::json();
    return j;
}

}  // namespace pptctl
