#include "query.h"
#include "schema.h"
#include "column_rw.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>

// ── Tokenizer ─────────────────────────────────────────────────────────────────

struct Tokens {
    std::vector<std::string> toks;
    size_t pos = 0;

    bool eof()  const { return pos >= toks.size(); }
    const std::string& peek() const { static std::string empty; return eof() ? empty : toks[pos]; }
    std::string next() { return eof() ? "" : toks[pos++]; }

    bool expect(const std::string& s, std::string& err) {
        if (peek() == s) { ++pos; return true; }
        err = "Expected '" + s + "', got '" + peek() + "'";
        return false;
    }
};

static Tokens tokenize(const std::string& text) {
    Tokens t;
    std::string cur;
    bool in_str = false;

    auto flush = [&]() {
        if (!cur.empty()) { t.toks.push_back(cur); cur.clear(); }
    };

    for (char c : text) {
        if (in_str) {
            if (c == '\'') { cur += c; flush(); in_str = false; }
            else cur += c;
        } else if (c == '\'') {
            flush(); cur += c; in_str = true;
        } else if (c == ',' || c == '(' || c == ')') {
            flush(); t.toks.push_back(std::string(1, c));
        } else if (std::isspace(c)) {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return t;
}

// ── Parser ────────────────────────────────────────────────────────────────────

static AggFunc parse_agg(const std::string& s) {
    if (s == "COUNT") return AggFunc::COUNT;
    if (s == "SUM")   return AggFunc::SUM;
    if (s == "AVG")   return AggFunc::AVG;
    if (s == "MIN")   return AggFunc::MIN;
    if (s == "MAX")   return AggFunc::MAX;
    return AggFunc::NONE;
}

static CmpOp parse_op(const std::string& s, std::string& err) {
    if (s == "=")  return CmpOp::EQ;
    if (s == "!=") return CmpOp::NE;
    if (s == "<")  return CmpOp::LT;
    if (s == "<=") return CmpOp::LE;
    if (s == ">")  return CmpOp::GT;
    if (s == ">=") return CmpOp::GE;
    err = "Unknown operator: " + s;
    return CmpOp::EQ;
}

bool parse_query(const std::string& text, Query& q, std::string& err) {
    auto t = tokenize(text);

    // SELECT
    if (!t.expect("SELECT", err)) return false;

    // select_list
    while (true) {
        std::string tok = t.next();
        if (tok == "*") {
            SelectExpr e; e.star = true;
            q.select.push_back(e);
        } else {
            AggFunc agg = parse_agg(tok);
            if (agg != AggFunc::NONE) {
                // Expect (col) or (*)
                if (!t.expect("(", err)) return false;
                std::string arg = t.next();
                if (!t.expect(")", err)) return false;
                SelectExpr e;
                if (arg == "*") { e.count_star = true; }
                else            { e.column = arg; }
                e.agg = agg;
                q.select.push_back(e);
            } else {
                SelectExpr e; e.column = tok;
                q.select.push_back(e);
            }
        }
        if (t.peek() == ",") { t.next(); continue; }
        break;
    }

    // FROM
    if (!t.expect("FROM", err)) return false;
    q.table = t.next();

    // Optional WHERE
    if (!t.eof() && t.peek() == "WHERE") {
        t.next();
        Predicate p;
        p.col     = t.next();
        p.op      = parse_op(t.next(), err);
        p.literal = t.next();
        // Strip surrounding quotes from string literals
        if (!p.literal.empty() && p.literal.front() == '\'')
            p.literal = p.literal.substr(1, p.literal.size() - 2);
        q.where = p;
    }

    // Optional GROUP BY
    if (!t.eof() && t.peek() == "GROUP") {
        t.next();
        if (!t.expect("BY", err)) return false;
        q.group_by = t.next();
    }

    return true;
}

// ── Executor ──────────────────────────────────────────────────────────────────

// Evaluate a single predicate for one row value.
static bool eval_pred(const Value& v, CmpOp op, const Value& lit) {
    int cmp = compare_values(v, lit);
    switch (op) {
        case CmpOp::EQ: return cmp == 0;
        case CmpOp::NE: return cmp != 0;
        case CmpOp::LT: return cmp <  0;
        case CmpOp::LE: return cmp <= 0;
        case CmpOp::GT: return cmp >  0;
        case CmpOp::GE: return cmp >= 0;
    }
    return false;
}

bool execute_query(const Query& q, const std::string& warehouse_dir) {
    namespace fs = std::filesystem;
    auto t0 = std::chrono::steady_clock::now();

    // ── Locate table ───────────────────────────────────────────────────────────
    std::string tbl_dir = warehouse_dir + "/" + q.table;
    TableSchema schema;
    if (!read_schema(tbl_dir, schema)) {
        std::cerr << "Table not found: " << q.table << "\n";
        return false;
    }

    uint64_t N = 0;  // will be set after first column read

    // ── Determine which columns we actually need ────────────────────────────────
    // Track which column names are referenced so we can report I/O.
    std::vector<std::string> needed_cols;

    // Predicate column
    if (q.where.has_value()) needed_cols.push_back(q.where->col);

    // Select / group-by columns
    bool is_star = false;
    for (const auto& e : q.select) {
        if (e.star) { is_star = true; break; }
        if (!e.column.empty()) needed_cols.push_back(e.column);
    }
    if (!q.group_by.empty()) needed_cols.push_back(q.group_by);
    if (is_star) {
        for (const auto& c : schema.columns) needed_cols.push_back(c.name);
    }

    // Deduplicate
    std::sort(needed_cols.begin(), needed_cols.end());
    needed_cols.erase(std::unique(needed_cols.begin(), needed_cols.end()), needed_cols.end());

    // ── Build predicate bitmap ─────────────────────────────────────────────────
    // bitmap[i] = true if row i passes the WHERE clause (or always true if no WHERE)
    std::vector<bool> bitmap;

    if (q.where.has_value()) {
        const auto& pred = *q.where;
        int ci = find_column(schema, pred.col);
        if (ci < 0) {
            std::cerr << "WHERE column not found: " << pred.col << "\n";
            return false;
        }

        // Convert the literal string to the right type once
        Value lit;
        if (!parse_value(pred.literal, schema.columns[ci].type, lit)) {
            std::cerr << "Cannot parse literal: " << pred.literal << "\n";
            return false;
        }

        std::vector<Value> pred_col;
        if (!read_column(tbl_dir, schema.columns[ci], pred_col)) return false;
        N = pred_col.size();
        bitmap.resize(N);
        for (uint64_t i = 0; i < N; ++i)
            bitmap[i] = eval_pred(pred_col[i], pred.op, lit);
    }

    // If there's no WHERE, we need N from some column.
    if (N == 0 && !schema.columns.empty()) {
        std::string path = tbl_dir + "/" + schema.columns[0].name + ".col";
        std::ifstream f(path, std::ios::binary);
        ColHeader hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        N = hdr.row_count;
    }
    if (bitmap.empty()) bitmap.assign(N, true);

    uint64_t matching = 0;
    for (bool b : bitmap) if (b) ++matching;

    // ── Handle COUNT(*) — no column reads needed ───────────────────────────────
    bool count_star_only = (q.select.size() == 1 &&
                            q.select[0].count_star && q.group_by.empty());
    if (count_star_only) {
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "count\n-------\n" << matching << "\n";
        size_t pred_cols = q.where.has_value() ? 1 : 0;
        std::cout << "\n(" << matching << " rows, "
                  << std::fixed << std::setprecision(0) << ms << " ms)\n";
        std::cout << "[read " << pred_cols << " of " << schema.columns.size()
                  << " columns]\n";
        return true;
    }

    // ── SELECT * (point lookup / filter rows) ──────────────────────────────────
    if (is_star) {
        // Load every column
        std::vector<std::vector<Value>> cols(schema.columns.size());
        for (size_t ci = 0; ci < schema.columns.size(); ++ci)
            if (!read_column(tbl_dir, schema.columns[ci], cols[ci])) return false;

        // Print header
        for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
            std::cout << schema.columns[ci].name;
            if (ci + 1 < schema.columns.size()) std::cout << " | ";
        }
        std::cout << "\n";

        uint64_t rows_printed = 0;
        for (uint64_t i = 0; i < N; ++i) {
            if (!bitmap[i]) continue;
            for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
                std::cout << value_to_string(cols[ci][i]);
                if (ci + 1 < schema.columns.size()) std::cout << " | ";
            }
            std::cout << "\n";
            ++rows_printed;
        }

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "\n(" << rows_printed << " rows, "
                  << std::fixed << std::setprecision(0) << ms << " ms)\n";
        std::cout << "[read " << schema.columns.size() << " of "
                  << schema.columns.size() << " columns]\n";
        if (matching == 1)
            std::cout << "note: point lookups read every column, "
                         "which is slower than a row store.\n";
        return true;
    }

    // ── Aggregate (with optional GROUP BY) ────────────────────────────────────

    struct AggState {
        double  sum   = 0;
        double  min   = std::numeric_limits<double>::max();
        double  max   = std::numeric_limits<double>::lowest();
        int64_t count = 0;
    };

    // "default" group key = "" for queries without GROUP BY
    std::unordered_map<std::string, AggState> groups;

    // Load grouping column if needed
    std::vector<Value> grp_col;
    if (!q.group_by.empty()) {
        int ci = find_column(schema, q.group_by);
        if (ci < 0) { std::cerr << "GROUP BY column not found\n"; return false; }
        if (!read_column(tbl_dir, schema.columns[ci], grp_col)) return false;
    }

    // For each SelectExpr that has an aggregate over a column, load and aggregate.
    // We only support one aggregate expression per query for Phase 1.
    for (const auto& expr : q.select) {
        if (expr.agg == AggFunc::NONE || expr.count_star) continue;

        int ci = find_column(schema, expr.column);
        if (ci < 0) { std::cerr << "Column not found: " << expr.column << "\n"; return false; }

        std::vector<Value> agg_col;
        if (!read_column(tbl_dir, schema.columns[ci], agg_col)) return false;

        for (uint64_t i = 0; i < N; ++i) {
            if (!bitmap[i]) continue;
            std::string key = grp_col.empty() ? "" : value_to_string(grp_col[i]);
            auto& st = groups[key];
            double v = to_double(agg_col[i]);
            st.sum += v;
            if (v < st.min) st.min = v;
            if (v > st.max) st.max = v;
            ++st.count;
        }
    }

    // Handle COUNT(col) — just count non-null rows per group (all rows here)
    bool is_count = (q.select.size() == 1 && q.select[0].agg == AggFunc::COUNT);
    if (is_count && !q.select[0].count_star) {
        int ci = find_column(schema, q.select[0].column);
        if (ci < 0) { std::cerr << "Column not found\n"; return false; }
        std::vector<Value> col;
        if (!read_column(tbl_dir, schema.columns[ci], col)) return false;
        for (uint64_t i = 0; i < N; ++i) {
            if (!bitmap[i]) continue;
            std::string key = grp_col.empty() ? "" : value_to_string(grp_col[i]);
            groups[key].count++;
        }
    }

    // ── Print results ──────────────────────────────────────────────────────────
    // Sort groups for deterministic output
    std::vector<std::pair<std::string, AggState>> sorted_groups(groups.begin(), groups.end());
    std::sort(sorted_groups.begin(), sorted_groups.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    // Header
    if (!q.group_by.empty()) std::cout << q.group_by << " | ";
    for (const auto& e : q.select) {
        if (e.count_star) std::cout << "count(*)";
        else if (e.agg != AggFunc::NONE) {
            std::string fn;
            switch (e.agg) {
                case AggFunc::SUM: fn = "sum"; break;
                case AggFunc::AVG: fn = "avg"; break;
                case AggFunc::MIN: fn = "min"; break;
                case AggFunc::MAX: fn = "max"; break;
                case AggFunc::COUNT: fn = "count"; break;
                default: break;
            }
            std::cout << fn << "_" << e.column;
        } else {
            std::cout << e.column;
        }
    }
    std::cout << "\n";
    std::cout << std::string(40, '-') << "\n";

    for (const auto& [key, st] : sorted_groups) {
        if (!q.group_by.empty()) std::cout << key << " | ";
        for (const auto& e : q.select) {
            std::cout << std::fixed << std::setprecision(2);
            if (e.count_star || e.agg == AggFunc::COUNT)
                std::cout << st.count;
            else if (e.agg == AggFunc::SUM) std::cout << st.sum;
            else if (e.agg == AggFunc::AVG) std::cout << (st.count ? st.sum / st.count : 0.0);
            else if (e.agg == AggFunc::MIN) std::cout << st.min;
            else if (e.agg == AggFunc::MAX) std::cout << st.max;
        }
        std::cout << "\n";
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "\n(" << sorted_groups.size() << " rows, "
              << std::fixed << std::setprecision(0) << ms << " ms)\n";
    std::cout << "[read " << needed_cols.size() << " of "
              << schema.columns.size() << " columns]\n";
    return true;
}