// Diff engine — table-level ops. See diff_internal.hpp.
//
// Table shapes are paired by shape id, then by name (groups walked). For each
// pair the column counts are compared first, then rows are aligned by LCS
// over their cell text (the common prefix of columns only), and the rows left
// in each gap are paired by how many cells they share — positionally as a
// last resort — so an inserted row and an edited row below it come out as one
// rows_added and one cell_changed, not a cascade of changed cells.

#include <algorithm>
#include <cstddef>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

// ── matching ─────────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void collect_tables(const std::vector<Shape>& shapes, std::vector<const Shape*>& out) {
    for (const auto& s : shapes) {
        if (s.kind == ShapeKind::Table && s.table) {
            out.push_back(&s);
        }
        if (!s.children.empty()) {
            collect_tables(s.children, out);
        }
    }
}

struct Match {
    const Shape* a;
    const Shape* b;
};

// Pairs A's tables with B's: first by shape id, then by name among the
// leftovers (first come, first served, in document order). Unpaired tables
// are not reported here — the shape diff lists them as added/removed.
std::vector<Match> match_tables(const std::vector<const Shape*>& a,
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
        if (a_partner[i] != nullptr) {
            out.push_back({a[i], a_partner[i]});
        }
    }
    return out;
}

// ── normalization ────────────────────────────────────────────────────────────

bool is_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

// A cell reduced to what the diff compares: its paragraphs joined by '\n',
// with field runs dropped and whitespace collapsed unless strict. Mirrors the
// paragraph normalization of diff_text.cpp at cell granularity.
std::string normalize_cell(const TableCell& cell, bool strict) {
    if (strict) {
        return cell.text();
    }
    std::string out;
    for (const auto& para : cell.paragraphs) {
        std::string p;
        bool pending_space = false;
        for (const auto& r : para.runs) {
            if (r.is_field) {
                continue;
            }
            for (const char ch : r.text) {
                if (is_space(ch)) {
                    pending_space = true;
                    continue;
                }
                if (pending_space && !p.empty()) {
                    p.push_back(' ');
                }
                pending_space = false;
                p.push_back(ch);
            }
        }
        if (p.empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += p;
    }
    return out;
}

TableMerge merge_of(const TableCell& cell) noexcept {
    return TableMerge{cell.col_span, cell.row_span, cell.merged};
}

// One row's compared cells, over the common column prefix only.
struct Row {
    std::size_t index = 0;  // 1-based in its table
    std::vector<std::string> cells;
};

std::vector<Row> normalize_rows(const Table& table, std::size_t columns, bool strict) {
    std::vector<Row> out;
    out.reserve(table.rows.size());
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        Row r;
        r.index = i + 1;
        const auto& cells = table.rows[i].cells;
        for (std::size_t c = 0; c < columns && c < cells.size(); ++c) {
            r.cells.push_back(normalize_cell(cells[c], strict));
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::size_t column_count(const Table& t) noexcept {
    if (t.columns != 0) {
        return t.columns;
    }
    std::size_t n = 0;
    for (const auto& r : t.rows) {
        n = std::max(n, r.cells.size());
    }
    return n;
}

// ── row alignment ────────────────────────────────────────────────────────────

// Indices of LCS-aligned (a, b) rows over the compared cells, in order.
std::vector<std::pair<std::size_t, std::size_t>> align(const std::vector<Row>& a,
                                                       const std::vector<Row>& b) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::vector<std::size_t>> lcs(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i].cells == b[j].cells ? lcs[i + 1][j + 1] + 1
                                                 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i].cells == b[j].cells) {
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

std::size_t shared_cells(const Row& x, const Row& y) noexcept {
    std::size_t n = 0;
    for (std::size_t c = 0; c < x.cells.size() && c < y.cells.size(); ++c) {
        if (x.cells[c] == y.cells[c]) {
            ++n;
        }
    }
    return n;
}

// ── comparison ───────────────────────────────────────────────────────────────

struct Owner {
    std::string shape_id;
    std::string shape_name;
};

TableChange make_change(const Owner& owner, TableOp op) {
    TableChange c;
    c.op = op;
    c.shape_id = owner.shape_id;
    c.shape_name = owner.shape_name;
    return c;
}

// Cell-by-cell ops for two rows that are considered the same row.
void diff_row(const Owner& owner,
              const TableRow& a_row,
              const TableRow& b_row,
              const Row& a,
              const Row& b,
              std::vector<TableChange>& out) {
    const std::size_t n = std::min(a.cells.size(), b.cells.size());
    for (std::size_t c = 0; c < n; ++c) {
        if (a.cells[c] != b.cells[c]) {
            auto ch = make_change(owner, TableOp::CellChanged);
            ch.a_row = a.index;
            ch.b_row = b.index;
            ch.col = c + 1;
            ch.old_text = a_row.cells[c].text();
            ch.new_text = b_row.cells[c].text();
            out.push_back(std::move(ch));
        }
        const TableMerge am = merge_of(a_row.cells[c]);
        const TableMerge bm = merge_of(b_row.cells[c]);
        if (am != bm) {
            auto ch = make_change(owner, TableOp::MergeChanged);
            ch.a_row = a.index;
            ch.b_row = b.index;
            ch.col = c + 1;
            ch.old_merge = am;
            ch.new_merge = bm;
            out.push_back(std::move(ch));
        }
    }
}

std::vector<std::size_t> range(std::size_t from, std::size_t to) {
    std::vector<std::size_t> out;
    for (std::size_t i = from; i <= to; ++i) {
        out.push_back(i);
    }
    return out;
}

void diff_table_pair(const Shape& sa, const Shape& sb, bool strict, std::vector<TableChange>& out) {
    if (!sa.table || !sb.table) {
        return;
    }
    const Table& ta = *sa.table;
    const Table& tb = *sb.table;
    const Owner owner{sb.id, sb.name};

    const std::size_t ca = column_count(ta);
    const std::size_t cb = column_count(tb);
    if (cb > ca) {
        auto ch = make_change(owner, TableOp::ColumnsAdded);
        ch.indices = range(ca + 1, cb);
        out.push_back(std::move(ch));
    } else if (ca > cb) {
        auto ch = make_change(owner, TableOp::ColumnsRemoved);
        ch.indices = range(cb + 1, ca);
        out.push_back(std::move(ch));
    }
    const std::size_t columns = std::min(ca, cb);

    const auto a = normalize_rows(ta, columns, strict);
    const auto b = normalize_rows(tb, columns, strict);
    auto pairs = align(a, b);
    pairs.emplace_back(a.size(), b.size());  // sentinel: flush the trailing gap

    std::vector<std::size_t> removed;
    std::vector<std::size_t> added;
    std::vector<std::pair<std::size_t, std::size_t>> edited;  // (a, b) row indices

    // Rows left in one gap: pair each A row with the unclaimed B row it shares
    // the most cells with, then pair the leftovers positionally (a rewritten
    // row is still a row), and what remains is removed/added.
    const auto flush_gap =
        [&](std::size_t a_from, std::size_t a_to, std::size_t b_from, std::size_t b_to) {
            std::vector<bool> b_used(b_to - b_from, false);
            std::vector<bool> a_used(a_to - a_from, false);
            for (std::size_t i = a_from; i < a_to; ++i) {
                std::size_t best = 0;
                std::size_t best_j = b_to;
                for (std::size_t j = b_from; j < b_to; ++j) {
                    if (b_used[j - b_from]) {
                        continue;
                    }
                    const std::size_t s = shared_cells(a[i], b[j]);
                    if (s > best) {
                        best = s;
                        best_j = j;
                    }
                }
                if (best_j != b_to) {
                    b_used[best_j - b_from] = true;
                    a_used[i - a_from] = true;
                    edited.emplace_back(i, best_j);
                }
            }
            std::size_t j = b_from;
            for (std::size_t i = a_from; i < a_to; ++i) {
                if (a_used[i - a_from]) {
                    continue;
                }
                while (j < b_to && b_used[j - b_from]) {
                    ++j;
                }
                if (j < b_to) {
                    b_used[j - b_from] = true;
                    edited.emplace_back(i, j);
                    ++j;
                } else {
                    removed.push_back(a[i].index);
                }
            }
            for (std::size_t k = b_from; k < b_to; ++k) {
                if (!b_used[k - b_from]) {
                    added.push_back(b[k].index);
                }
            }
        };

    std::size_t ai = 0;
    std::size_t bi = 0;
    for (const auto& [pa, pb] : pairs) {
        flush_gap(ai, pa, bi, pb);
        if (pa < a.size()) {
            edited.emplace_back(pa, pb);
        }
        ai = pa + 1;
        bi = pb + 1;
    }

    if (!removed.empty()) {
        auto ch = make_change(owner, TableOp::RowsRemoved);
        ch.indices = std::move(removed);
        out.push_back(std::move(ch));
    }
    if (!added.empty()) {
        auto ch = make_change(owner, TableOp::RowsAdded);
        ch.indices = std::move(added);
        out.push_back(std::move(ch));
    }
    std::sort(edited.begin(), edited.end());
    for (const auto& [i, j] : edited) {
        diff_row(owner, ta.rows[i], tb.rows[j], a[i], b[j], out);
    }
}

// ── rendering ────────────────────────────────────────────────────────────────

std::string join_indices(const std::vector<std::size_t>& v) {
    std::string out;
    for (const auto i : v) {
        if (!out.empty()) {
            out += ", ";
        }
        out += std::to_string(i);
    }
    return out;
}

// "2 rows added (rows 3, 4)" / "1 column removed (column 2)".
std::string count_label(const TableChange& c, std::string_view unit, std::string_view verb) {
    const std::size_t n = c.indices.size();
    return std::format("{} {}{} {} ({}{} {})",
                       n,
                       unit,
                       n == 1 ? "" : "s",
                       verb,
                       unit,
                       n == 1 ? "" : "s",
                       join_indices(c.indices));
}

std::string merge_label(const std::optional<TableMerge>& m) {
    if (!m) {
        return "?";
    }
    return std::format("{}x{}{}", m->col_span, m->row_span, m->covered ? " (covered)" : "");
}

nlohmann::json merge_to_json(const std::optional<TableMerge>& m) {
    if (!m) {
        return nullptr;
    }
    return {{"col_span", m->col_span}, {"row_span", m->row_span}, {"covered", m->covered}};
}

nlohmann::json index_or_null(std::size_t i) {
    return i == 0 ? nlohmann::json(nullptr) : nlohmann::json(i);
}

bool is_cell_op(TableOp op) noexcept {
    return op == TableOp::CellChanged || op == TableOp::MergeChanged;
}

}  // namespace

namespace detail {

void diff_tables(const Slide& a,
                 const Slide& b,
                 const DiffOptions& opts,
                 std::vector<TableChange>& out) {
    std::vector<const Shape*> ta;
    std::vector<const Shape*> tb;
    collect_tables(a.shapes(), ta);
    collect_tables(b.shapes(), tb);
    for (const auto& m : match_tables(ta, tb)) {
        diff_table_pair(*m.a, *m.b, opts.strict_text, out);
    }
}

}  // namespace detail

std::string_view to_string(TableOp op) noexcept {
    switch (op) {
        case TableOp::CellChanged:
            return "cell_changed";
        case TableOp::RowsAdded:
            return "rows_added";
        case TableOp::RowsRemoved:
            return "rows_removed";
        case TableOp::ColumnsAdded:
            return "columns_added";
        case TableOp::ColumnsRemoved:
            return "columns_removed";
        case TableOp::MergeChanged:
            return "merge_changed";
    }
    return "unknown";
}

std::string describe(const TableChange& change) {
    std::string head = std::format("table \"{}\"", change.shape_name);
    const std::string now = change.a_row != change.b_row
                                ? std::format(" (now ({},{}))", change.b_row, change.col)
                                : std::string{};
    switch (change.op) {
        case TableOp::CellChanged:
            return std::format(R"({}: cell ({},{}){} changed: "{}" -> "{}")",
                               head,
                               change.a_row,
                               change.col,
                               now,
                               change.old_text,
                               change.new_text);
        case TableOp::RowsAdded:
            return std::format("{}: {}", head, count_label(change, "row", "added"));
        case TableOp::RowsRemoved:
            return std::format("{}: {}", head, count_label(change, "row", "removed"));
        case TableOp::ColumnsAdded:
            return std::format("{}: {}", head, count_label(change, "column", "added"));
        case TableOp::ColumnsRemoved:
            return std::format("{}: {}", head, count_label(change, "column", "removed"));
        case TableOp::MergeChanged:
            return std::format("{}: cell ({},{}){} merge changed: {} -> {}",
                               head,
                               change.a_row,
                               change.col,
                               now,
                               merge_label(change.old_merge),
                               merge_label(change.new_merge));
    }
    return head;
}

nlohmann::json to_json(const TableChange& change) {
    const bool cell = is_cell_op(change.op);
    nlohmann::json j;
    j["op"] = to_string(change.op);
    j["shape_id"] = change.shape_id;
    j["shape_name"] = change.shape_name;
    j["a_row"] = cell ? index_or_null(change.a_row) : nlohmann::json(nullptr);
    j["b_row"] = cell ? index_or_null(change.b_row) : nlohmann::json(nullptr);
    j["col"] = cell ? index_or_null(change.col) : nlohmann::json(nullptr);
    j["indices"] = cell ? nlohmann::json(nullptr) : nlohmann::json(change.indices);
    j["old_text"] = change.op == TableOp::CellChanged ? nlohmann::json(change.old_text)
                                                      : nlohmann::json(nullptr);
    j["new_text"] = change.op == TableOp::CellChanged ? nlohmann::json(change.new_text)
                                                      : nlohmann::json(nullptr);
    j["old_merge"] = change.op == TableOp::MergeChanged ? merge_to_json(change.old_merge) : nullptr;
    j["new_merge"] = change.op == TableOp::MergeChanged ? merge_to_json(change.new_merge) : nullptr;
    return j;
}

}  // namespace pptctl
