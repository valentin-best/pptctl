// Diff engine: slide summaries, similarity, and slide matching.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <pptctl/diff.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

// What identifies a picture's image for comparison: the media content hash
// when the slide was loaded from a package, so the same bytes match across
// decks whatever part name they sit under and an image swap alone marks the
// slide changed; else the resolved part; else the rId (parse-only slides).
std::string picture_identity(const Picture& p) {
    if (p.media_hash) {
        return std::format("{:016x}", *p.media_hash);
    }
    return p.target.empty() ? p.rel_id : p.target;
}

// ── fingerprint ───────────────────────────────────────────────────────────────
//
// FNV-1a over a canonical serialization of everything the diff compares. Any
// property a later op kind reports (geometry, run formatting, picture target,
// table cells, chart data, notes) is folded in, so "fingerprints equal" is a
// sound "unchanged". Field separators keep "ab"+"c" distinct from "a"+"bc".

class Fnv1a {
 public:
    void feed(std::string_view s) noexcept {
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            h_ ^= c;
            h_ *= kPrime;
        }
        // Separator so adjacent fields can't run together.
        h_ ^= 0xFFU;
        h_ *= kPrime;
    }
    void feed(std::int64_t v) {
        feed(std::to_string(v));
    }
    void feed(int v) {
        feed(static_cast<std::int64_t>(v));
    }
    void feed(bool b) noexcept {
        feed(b ? std::string_view{"1"} : std::string_view{"0"});
    }
    template <typename T>
    void feed(const std::optional<T>& v) {
        if (v) {
            feed(*v);
        } else {
            feed(std::string_view{"-"});
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return h_;
    }

 private:
    static constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t h_ = kOffset;
};

void feed_paragraphs(Fnv1a& h, const std::vector<Paragraph>& paras) {
    for (const auto& p : paras) {
        h.feed(std::string_view{"p"});
        h.feed(static_cast<std::int64_t>(p.level));
        for (const auto& r : p.runs) {
            h.feed(r.text);
            h.feed(r.is_field);
            h.feed(r.props.bold);
            h.feed(r.props.italic);
            h.feed(r.props.underlined);
            h.feed(r.props.size_centipt);
            h.feed(r.props.font);
            h.feed(r.props.font_ea);
            h.feed(r.props.font_cs);
        }
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void feed_shape(Fnv1a& h, const Shape& s) {
    h.feed(std::string_view{"shape"});
    h.feed(static_cast<std::int64_t>(s.kind));
    h.feed(s.id);
    h.feed(s.name);
    h.feed(s.placeholder_type);
    h.feed(s.placeholder_idx);
    if (s.geometry) {
        h.feed(s.geometry->off_x);
        h.feed(s.geometry->off_y);
        h.feed(s.geometry->ext_cx);
        h.feed(s.geometry->ext_cy);
    } else {
        h.feed(std::string_view{"nogeom"});
    }
    if (s.fill) {
        h.feed(s.fill->kind);
        h.feed(s.fill->color);
    } else {
        h.feed(std::string_view{"nofill"});
    }
    feed_paragraphs(h, s.paragraphs);
    if (s.picture) {
        h.feed(picture_identity(*s.picture));
        h.feed(s.picture->linked);
        h.feed(s.picture->alt_text);
        h.feed(s.picture->title);
        h.feed(s.picture->decorative);
    }
    if (s.table) {
        h.feed(static_cast<std::int64_t>(s.table->columns));
        for (const auto& row : s.table->rows) {
            h.feed(std::string_view{"row"});
            for (const auto& cell : row.cells) {
                h.feed(static_cast<std::int64_t>(cell.col_span));
                h.feed(static_cast<std::int64_t>(cell.row_span));
                h.feed(cell.merged);
                feed_paragraphs(h, cell.paragraphs);
            }
        }
    }
    if (s.chart) {
        h.feed(s.chart->chart_type);
        h.feed(s.chart->title);
        for (const auto& ser : s.chart->series) {
            h.feed(ser.name);
            for (const auto& c : ser.categories) {
                h.feed(c);
            }
            for (const double v : ser.values) {
                h.feed(std::format("{}", v));
            }
        }
    }
    for (const auto& child : s.children) {
        feed_shape(h, child);
    }
}

// ── tokens ────────────────────────────────────────────────────────────────────
//
// Words of every piece of text on the slide plus one token per picture target.
// Shape names are deliberately left out: default placeholder names ("Title 1")
// appear on every slide and would give unrelated slides a baseline score.

bool is_word_byte(unsigned char c) noexcept {
    // Keep multi-byte UTF-8 sequences intact by treating every byte ≥ 0x80 as
    // part of a word; only ASCII punctuation and whitespace split.
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void tokenize_into(std::string_view text, std::vector<std::string>& out) {
    std::string cur;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (is_word_byte(c)) {
            cur.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) {
        out.push_back(std::move(cur));
    }
}

void tokenize_paragraphs(const std::vector<Paragraph>& paras, std::vector<std::string>& out) {
    for (const auto& p : paras) {
        tokenize_into(p.text(), out);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void tokenize_shape(const Shape& s, std::vector<std::string>& out) {
    tokenize_paragraphs(s.paragraphs, out);
    if (s.picture) {
        out.push_back("pic:" + picture_identity(*s.picture));
    }
    if (s.table) {
        for (const auto& row : s.table->rows) {
            for (const auto& cell : row.cells) {
                tokenize_paragraphs(cell.paragraphs, out);
            }
        }
    }
    if (s.chart) {
        if (s.chart->title) {
            tokenize_into(*s.chart->title, out);
        }
        for (const auto& ser : s.chart->series) {
            tokenize_into(ser.name, out);
        }
    }
    for (const auto& child : s.children) {
        tokenize_shape(child, out);
    }
}

// ── matching ──────────────────────────────────────────────────────────────────

struct Pair {
    std::size_t a;  // 1-based
    std::size_t b;  // 1-based
    double similarity;
    bool exact;  // fingerprints equal → unchanged; else best-match → changed
    bool moved = false;
};

// Indices (into `pairs`, which is sorted by a) that form a longest increasing
// subsequence of b. Everything outside it moved relative to its peers.
std::vector<bool> in_longest_increasing_run(const std::vector<Pair>& pairs) {
    const std::size_t n = pairs.size();
    std::vector<std::size_t> tail;  // tail[k] = index of smallest b ending a run of length k+1
    std::vector<std::size_t> prev(n, n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto it =
            std::lower_bound(tail.begin(), tail.end(), i, [&](std::size_t x, std::size_t y) {
                return pairs[x].b < pairs[y].b;
            });
        if (it != tail.begin()) {
            prev[i] = *(it - 1);
        }
        if (it == tail.end()) {
            tail.push_back(i);
        } else {
            *it = i;
        }
    }
    std::vector<bool> keep(n, false);
    if (!tail.empty()) {
        for (std::size_t i = tail.back(); i != n; i = prev[i]) {
            keep[i] = true;
        }
    }
    return keep;
}

}  // namespace

SlideSummary summarize(const Slide& slide) {
    SlideSummary s;
    s.index = slide.index();

    Fnv1a h;
    for (const auto& shape : slide.shapes()) {
        feed_shape(h, shape);
    }
    h.feed(std::string_view{"notes"});
    feed_paragraphs(h, slide.notes());
    s.fingerprint = h.value();

    for (const auto& shape : slide.shapes()) {
        tokenize_shape(shape, s.tokens);
    }
    tokenize_paragraphs(slide.notes(), s.tokens);
    std::sort(s.tokens.begin(), s.tokens.end());
    return s;
}

double slide_similarity(const SlideSummary& a, const SlideSummary& b) {
    const std::size_t total = a.tokens.size() + b.tokens.size();
    if (total == 0) {
        return 1.0;
    }
    // Multiset intersection of two sorted vectors.
    std::size_t common = 0;
    auto i = a.tokens.begin();
    auto j = b.tokens.begin();
    while (i != a.tokens.end() && j != b.tokens.end()) {
        if (*i < *j) {
            ++i;
        } else if (*j < *i) {
            ++j;
        } else {
            ++common;
            ++i;
            ++j;
        }
    }
    return 2.0 * static_cast<double>(common) / static_cast<double>(total);
}

bool DiffReport::identical() const noexcept {
    return std::all_of(slides.begin(), slides.end(), [](const SlideDiff& s) {
        return s.status == SlideStatus::Unchanged && !s.moved;
    });
}

DiffReport match_slides(const std::vector<SlideSummary>& a,
                        const std::vector<SlideSummary>& b,
                        const DiffOptions& opts) {
    std::vector<Pair> pairs;
    std::vector<bool> a_used(a.size(), false);
    std::vector<bool> b_used(b.size(), false);

    // 1. Exact fingerprint matches, first-come in A order. Duplicate slides
    //    (same fingerprint several times) pair up in order.
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> by_fp;
    for (std::size_t j = b.size(); j-- > 0;) {
        by_fp[b[j].fingerprint].push_back(j);  // reversed so back() is the lowest index
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto it = by_fp.find(a[i].fingerprint);
        if (it == by_fp.end() || it->second.empty()) {
            continue;
        }
        const std::size_t j = it->second.back();
        it->second.pop_back();
        a_used[i] = true;
        b_used[j] = true;
        pairs.push_back({a[i].index, b[j].index, 1.0, true});
    }

    // 2. Best-match the leftovers: repeatedly take the most similar unmatched
    //    (A, B) pair while it clears the threshold.
    for (;;) {
        double best = -1.0;
        std::size_t bi = 0;
        std::size_t bj = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a_used[i]) {
                continue;
            }
            for (std::size_t j = 0; j < b.size(); ++j) {
                if (b_used[j]) {
                    continue;
                }
                const double s = slide_similarity(a[i], b[j]);
                if (s > best) {
                    best = s;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (best < opts.match_threshold) {
            break;
        }
        a_used[bi] = true;
        b_used[bj] = true;
        pairs.push_back({a[bi].index, b[bj].index, best, false});
    }

    // 3. Moves: pairs whose B order disagrees with their A order.
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.a < y.a; });
    const auto keep = in_longest_increasing_run(pairs);
    for (std::size_t k = 0; k < pairs.size(); ++k) {
        pairs[k].moved = !keep[k];
    }

    // 4. Assemble in B order, then slot removed A slides in at their A position.
    DiffReport report;
    std::unordered_map<std::size_t, const Pair*> by_b;
    for (const auto& p : pairs) {
        by_b[p.b] = &p;
    }
    for (const auto& bs : b) {
        SlideDiff d;
        d.b_index = bs.index;
        if (const auto it = by_b.find(bs.index); it != by_b.end()) {
            const Pair& p = *it->second;
            d.a_index = p.a;
            d.moved = p.moved;
            d.similarity = p.similarity;
            d.status = p.exact ? SlideStatus::Unchanged : SlideStatus::Changed;
        } else {
            d.status = SlideStatus::Added;
            d.similarity = 0.0;
        }
        report.slides.push_back(d);
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a_used[i]) {
            continue;
        }
        SlideDiff d;
        d.status = SlideStatus::Removed;
        d.a_index = a[i].index;
        d.similarity = 0.0;
        // Slot it after the last slide that preceded it in A, and before the
        // first slide that followed it in A or was added there — so at one
        // position a removal reads before an addition, as in a text diff.
        auto start = report.slides.begin();
        for (auto it = report.slides.begin(); it != report.slides.end(); ++it) {
            if (it->a_index != 0 && it->a_index < a[i].index) {
                start = it + 1;
            }
        }
        const auto pos = std::find_if(start, report.slides.end(), [&](const SlideDiff& s) {
            return s.status == SlideStatus::Added || s.a_index > a[i].index;
        });
        report.slides.insert(pos, d);
    }
    return report;
}

SlideChanges compare_slides(const Slide& a, const Slide& b, const DiffOptions& opts) {
    SlideChanges out;
    detail::diff_text(a, b, opts, out.text);
    detail::diff_shapes(a, b, opts, out.shapes);
    detail::diff_pictures(a, b, opts, out.pictures);
    return out;
}

}  // namespace pptctl
