#include "row_store.h"
#include "csv_parser.h"
#include "column_rw.h"
#include "query.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <limits>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string row_path(const std::string& dir, const std::string& name) {
    return dir + "/" + name + ".rows";
}

template<typename T>
static void write_pod(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template<typename T>
static bool read_pod(std::ifstream& f, T& v) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

static void write_value(std::ostringstream& buf, const Value& v, DataType t) {
    uint8_t type_byte = static_cast<uint8_t>(t);
    buf.write(reinterpret_cast<const char*>(&type_byte), 1);

    switch (t) {
        case DataType::INT32: {
            int32_t x = std::get<int32_t>(v);
            uint32_t len = 4;
            buf.write(reinterpret_cast<const char*>(&len), 4);
            buf.write(reinterpret_cast<const char*>(&x), 4);
            break;
        }
        case DataType::INT64: {
            int64_t x = std::get<int64_t>(v);
            uint32_t len = 8;
            buf.write(reinterpret_cast<const char*>(&len), 4);
            buf.write(reinterpret_cast<const char*>(&x), 8);
            break;
        }
        case DataType::DOUBLE: {
            double x = std::get<double>(v);
            uint32_t len = 8;
            buf.write(reinterpret_cast<const char*>(&len), 4);
            buf.write(reinterpret_cast<const char*>(&x), 8);
            break;
        }
        case DataType::STRING: {
            const std::string& s = std::get<std::string>(v);
            uint32_t len = static_cast<uint32_t>(s.size());
            buf.write(reinterpret_cast<const char*>(&len), 4);
            buf.write(s.data(), len);
            break;
        }
    }
}

static bool read_value(std::istringstream& buf, DataType t, Value& v) {
    switch (t) {
        case DataType::INT32: {
            int32_t x; buf.read(reinterpret_cast<char*>(&x), 4); v = x; break;
        }
        case DataType::INT64: {
            int64_t x; buf.read(reinterpret_cast<char*>(&x), 8); v = x; break;
        }
        case DataType::DOUBLE: {
            double x; buf.read(reinterpret_cast<char*>(&x), 8); v = x; break;
        }
        case DataType::STRING: {
            uint32_t len;
            buf.read(reinterpret_cast<char*>(&len), 4);
            std::string s(len, '\0');
            buf.read(s.data(), len);
            v = std::move(s);
            break;
        }
    }
    return true;
}

// ── Load CSV into row store ──────────────────────────────────────────────────

bool row_load_csv(const std::string& csv_path,
                  const std::string& table_name,
                  const std::string& warehouse_dir) {
    CSVData csv;
    if (!load_csv(csv_path, csv)) return false;

    size_t num_cols = csv.headers.size();
    size_t num_rows = csv.rows.size();

    std::string tbl_dir = warehouse_dir + "/" + table_name;
    fs::create_directories(tbl_dir);

    std::string tmp  = row_path(tbl_dir, table_name) + ".tmp";
    std::string dest = row_path(tbl_dir, table_name);

    // Infer types
    std::vector<std::vector<std::string>> raw(num_cols);
    for (size_t r = 0; r < num_rows; ++r)
        for (size_t c = 0; c < num_cols; ++c)
            raw[c].push_back(csv.rows[r][c]);

    std::vector<DataType> types(num_cols);
    for (size_t c = 0; c < num_cols; ++c)
        types[c] = infer_type(raw[c]);

    // Build binary content
    std::ostringstream buf;

    // Header: column count (2 bytes), then per-column: [name_len:2][name...][type:1]
    uint16_t col_count = static_cast<uint16_t>(num_cols);
    buf.write(reinterpret_cast<const char*>(&col_count), 2);
    for (size_t c = 0; c < num_cols; ++c) {
        uint16_t name_len = static_cast<uint16_t>(csv.headers[c].size());
        uint8_t  type_byte = static_cast<uint8_t>(types[c]);
        buf.write(reinterpret_cast<const char*>(&name_len), 2);
        buf.write(csv.headers[c].data(), name_len);
        buf.write(reinterpret_cast<const char*>(&type_byte), 1);
    }

    // Row count
    uint64_t rc = num_rows;
    buf.write(reinterpret_cast<const char*>(&rc), 8);

    // Rows
    for (size_t r = 0; r < num_rows; ++r) {
        uint16_t cc = static_cast<uint16_t>(num_cols);
        buf.write(reinterpret_cast<const char*>(&cc), 2);
        for (size_t c = 0; c < num_cols; ++c) {
            Value v;
            parse_value(csv.rows[r][c], types[c], v);
            write_value(buf, v, types[c]);
        }
    }

    std::string data = buf.str();

    std::ofstream f(tmp, std::ios::binary);
    if (!f) { std::cerr << "Cannot write: " << tmp << "\n"; return false; }
    f.write(data.data(), data.size());
    f.close();

    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::cerr << "Cannot rename: " << tmp << "\n";
        return false;
    }

    std::cout << "Row store: loaded " << num_rows << " rows, "
              << num_cols << " columns into " << dest
              << " (" << (data.size() / 1e6) << " MB)\n";
    return true;
}

// ── Read row store info ──────────────────────────────────────────────────────

bool row_read_info(const std::string& dir,
                   const std::string& table_name,
                   RowTableInfo& info) {
    std::string path = row_path(dir, table_name);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Read content into memory for easy parsing
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    std::istringstream buf(content);

    uint16_t col_count;
    buf.read(reinterpret_cast<char*>(&col_count), 2);
    info.columns.resize(col_count);

    for (uint16_t i = 0; i < col_count; ++i) {
        uint16_t name_len;
        buf.read(reinterpret_cast<char*>(&name_len), 2);
        std::string name(name_len, '\0');
        buf.read(name.data(), name_len);
        uint8_t type_byte;
        buf.read(reinterpret_cast<char*>(&type_byte), 1);
        info.columns[i].name = name;
        info.columns[i].type = static_cast<DataType>(type_byte);
    }

    buf.read(reinterpret_cast<char*>(&info.row_count), 8);
    info.table_name = table_name;
    return true;
}

// ── Row store query executor ─────────────────────────────────────────────────

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

bool row_execute_query(const Query& q, const std::string& warehouse_dir) {
    auto t0 = std::chrono::steady_clock::now();

    std::string path = row_path(warehouse_dir + "/" + q.table, q.table);
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Row table not found: " << q.table << "\n"; return false; }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    std::istringstream buf(content);
    // Tell position for file size reporting
    size_t total_bytes = content.size();

    // Read header
    uint16_t num_cols;
    buf.read(reinterpret_cast<char*>(&num_cols), 2);

    std::vector<std::string> col_names(num_cols);
    std::vector<DataType> col_types(num_cols);
    for (uint16_t i = 0; i < num_cols; ++i) {
        uint16_t name_len;
        buf.read(reinterpret_cast<char*>(&name_len), 2);
        col_names[i].resize(name_len);
        buf.read(col_names[i].data(), name_len);
        uint8_t type_byte;
        buf.read(reinterpret_cast<char*>(&type_byte), 1);
        col_types[i] = static_cast<DataType>(type_byte);
    }

    uint64_t N;
    buf.read(reinterpret_cast<char*>(&N), 8);

    // Helper: find column index by name
    auto col_idx = [&](const std::string& name) -> int {
        for (uint16_t i = 0; i < num_cols; ++i)
            if (col_names[i] == name) return i;
        return -1;
    };

    // ── Determine needed columns ──────────────────────────────────────────────
    std::vector<int> needed_idxs;
    bool is_star = false;
    for (const auto& e : q.select) {
        if (e.star) { is_star = true; break; }
        if (!e.column.empty()) {
            int ci = col_idx(e.column);
            if (ci >= 0) needed_idxs.push_back(ci);
        }
    }
    if (is_star) {
        for (uint16_t i = 0; i < num_cols; ++i) needed_idxs.push_back(i);
    }

    int pred_idx = -1;
    Value lit_val;
    if (q.where.has_value()) {
        pred_idx = col_idx(q.where->col);
        if (pred_idx < 0) { std::cerr << "WHERE column not found\n"; return false; }
        parse_value(q.where->literal, col_types[pred_idx], lit_val);
    }

    int grp_idx = -1;
    if (!q.group_by.empty()) {
        grp_idx = col_idx(q.group_by);
    }

    // ── Scan rows ─────────────────────────────────────────────────────────────
    // Read offset of first row data
    size_t header_end = buf.tellg();

    // We'll read all row offsets first for simplicity
    std::vector<size_t> row_offsets;
    row_offsets.reserve(N);
    row_offsets.push_back(header_end);

    for (uint64_t i = 0; i < N; ++i) {
        buf.seekg(row_offsets[i]);
        uint16_t cc;
        buf.read(reinterpret_cast<char*>(&cc), 2);
        // Skip all columns
        for (uint16_t j = 0; j < cc; ++j) {
            uint8_t type_byte;
            buf.read(reinterpret_cast<char*>(&type_byte), 1);
            uint32_t len;
            buf.read(reinterpret_cast<char*>(&len), 4);
            buf.seekg(len, std::ios::cur);
        }
        if (i + 1 < N)
            row_offsets.push_back(buf.tellg());
    }

    // ── Build bitmap ──────────────────────────────────────────────────────────
    std::vector<bool> bitmap(N, true);
    uint64_t matching = N;

    if (q.where.has_value()) {
        matching = 0;
        for (uint64_t i = 0; i < N; ++i) {
            buf.seekg(row_offsets[i]);
            uint16_t cc; buf.read(reinterpret_cast<char*>(&cc), 2);
            // Skip to predicate column
            for (uint16_t j = 0; j < cc; ++j) {
                uint8_t type_byte;
                buf.read(reinterpret_cast<char*>(&type_byte), 1);
                uint32_t len;
                buf.read(reinterpret_cast<char*>(&len), 4);
                if (j == (uint16_t)pred_idx) {
                    Value v;
                    auto dt = static_cast<DataType>(type_byte);
                    switch (dt) {
                        case DataType::INT32:  { int32_t x;  buf.read(reinterpret_cast<char*>(&x), 4); v = x; break; }
                        case DataType::INT64:  { int64_t x;  buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                        case DataType::DOUBLE: { double x;   buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                        case DataType::STRING: {
                            std::string s(len, '\0');
                            buf.read(s.data(), len);
                            v = std::move(s);
                            break;
                        }
                    }
                    bitmap[i] = eval_pred(v, q.where->op, lit_val);
                    if (bitmap[i]) ++matching;
                    break;
                } else {
                    buf.seekg(len, std::ios::cur);
                }
            }
        }
    }

    // ── COUNT(*) short-circuit ────────────────────────────────────────────────
    bool count_star_only = (q.select.size() == 1 && q.select[0].count_star && q.group_by.empty());
    if (count_star_only) {
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "count\n-------\n" << matching << "\n";
        std::cout << "\n(" << matching << " rows, " << std::fixed << std::setprecision(0) << ms << " ms)\n";
        std::cout << "[row store: read " << (total_bytes / 1e6) << " MB]\n";
        return true;
    }

    // ── SELECT * ──────────────────────────────────────────────────────────────
    if (is_star) {
        for (uint16_t i = 0; i < num_cols; ++i) {
            std::cout << col_names[i];
            if (i + 1 < num_cols) std::cout << " | ";
        }
        std::cout << "\n";

        uint64_t printed = 0;
        for (uint64_t i = 0; i < N; ++i) {
            if (!bitmap[i]) continue;
            buf.seekg(row_offsets[i]);
            uint16_t cc; buf.read(reinterpret_cast<char*>(&cc), 2);
            for (uint16_t j = 0; j < cc; ++j) {
                uint8_t type_byte;
                buf.read(reinterpret_cast<char*>(&type_byte), 1);
                uint32_t len;
                buf.read(reinterpret_cast<char*>(&len), 4);
                Value v;
                auto dt = static_cast<DataType>(type_byte);
                switch (dt) {
                    case DataType::INT32:  { int32_t x;  buf.read(reinterpret_cast<char*>(&x), 4); v = x; break; }
                    case DataType::INT64:  { int64_t x;  buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                    case DataType::DOUBLE: { double x;   buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                    case DataType::STRING: {
                        std::string s(len, '\0');
                        buf.read(s.data(), len);
                        v = std::move(s);
                        break;
                    }
                }
                std::cout << value_to_string(v);
                if (j + 1 < cc) std::cout << " | ";
            }
            std::cout << "\n";
            ++printed;
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "\n(" << printed << " rows, " << std::fixed << std::setprecision(0) << ms << " ms)\n";
        std::cout << "[row store: read " << (total_bytes / 1e6) << " MB]\n";
        return true;
    }

    // ── Aggregates ────────────────────────────────────────────────────────────
    struct AggState {
        double sum = 0, min = std::numeric_limits<double>::max(), max = std::numeric_limits<double>::lowest();
        int64_t count = 0;
    };
    std::unordered_map<std::string, AggState> groups;

    // Read aggregate column
    for (const auto& expr : q.select) {
        if (expr.agg == AggFunc::NONE || expr.count_star) continue;
        int ci = col_idx(expr.column);
        if (ci < 0) continue;

        for (uint64_t i = 0; i < N; ++i) {
            if (!bitmap[i]) continue;
            buf.seekg(row_offsets[i]);
            uint16_t cc; buf.read(reinterpret_cast<char*>(&cc), 2);

            // Read group-by column first if needed
            std::string grp_key;
            if (grp_idx >= 0) {
                for (uint16_t j = 0; j < cc; ++j) {
                    uint8_t tb; buf.read(reinterpret_cast<char*>(&tb), 1);
                    uint32_t l; buf.read(reinterpret_cast<char*>(&l), 4);
                    if (j == (uint16_t)grp_idx) {
                        auto dt = static_cast<DataType>(tb);
                        switch (dt) {
                            case DataType::INT32:  { int32_t x;  buf.read(reinterpret_cast<char*>(&x), 4); grp_key = std::to_string(x); break; }
                            case DataType::INT64:  { int64_t x;  buf.read(reinterpret_cast<char*>(&x), 8); grp_key = std::to_string(x); break; }
                            case DataType::DOUBLE: { double x;   buf.read(reinterpret_cast<char*>(&x), 8); grp_key = std::to_string(x); break; }
                            case DataType::STRING: { std::string s(l, '\0'); buf.read(s.data(), l); grp_key = s; break; }
                        }
                        break;
                    } else {
                        buf.seekg(l, std::ios::cur);
                    }
                }
                // Re-seek to row start + 2
                buf.seekg(row_offsets[i] + 2);
            } else {
                buf.seekg(row_offsets[i] + 2);
            }

            // Now read the target aggregate column
            for (uint16_t j = 0; j < cc; ++j) {
                uint8_t tb; buf.read(reinterpret_cast<char*>(&tb), 1);
                uint32_t l; buf.read(reinterpret_cast<char*>(&l), 4);
                if (j == (uint16_t)ci) {
                    Value v;
                    auto dt = static_cast<DataType>(tb);
                    switch (dt) {
                        case DataType::INT32:  { int32_t x;  buf.read(reinterpret_cast<char*>(&x), 4); v = x; break; }
                        case DataType::INT64:  { int64_t x;  buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                        case DataType::DOUBLE: { double x;   buf.read(reinterpret_cast<char*>(&x), 8); v = x; break; }
                        case DataType::STRING: { std::string s(l, '\0'); buf.read(s.data(), l); v = s; break; }
                    }
                    auto& st = groups[grp_key];
                    double d = to_double(v);
                    st.sum += d;
                    if (d < st.min) st.min = d;
                    if (d > st.max) st.max = d;
                    ++st.count;
                    break;
                } else {
                    buf.seekg(l, std::ios::cur);
                }
            }
        }
    }

    // ── Print results ─────────────────────────────────────────────────────────
    std::vector<std::pair<std::string, AggState>> sorted(groups.begin(), groups.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    if (!q.group_by.empty()) std::cout << q.group_by << " | ";
    for (const auto& e : q.select) {
        if (e.count_star) std::cout << "count(*)";
        else if (e.agg != AggFunc::NONE) {
            switch (e.agg) {
                case AggFunc::SUM: std::cout << "sum_"; break;
                case AggFunc::AVG: std::cout << "avg_"; break;
                case AggFunc::MIN: std::cout << "min_"; break;
                case AggFunc::MAX: std::cout << "max_"; break;
                case AggFunc::COUNT: std::cout << "count_"; break;
                default: break;
            }
            std::cout << e.column;
        }
    }
    std::cout << "\n" << std::string(40, '-') << "\n";

    for (const auto& [key, st] : sorted) {
        if (!q.group_by.empty()) std::cout << key << " | ";
        for (const auto& e : q.select) {
            std::cout << std::fixed << std::setprecision(2);
            if (e.count_star || e.agg == AggFunc::COUNT) std::cout << st.count;
            else if (e.agg == AggFunc::SUM) std::cout << st.sum;
            else if (e.agg == AggFunc::AVG) std::cout << (st.count ? st.sum / st.count : 0);
            else if (e.agg == AggFunc::MIN) std::cout << st.min;
            else if (e.agg == AggFunc::MAX) std::cout << st.max;
        }
        std::cout << "\n";
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "\n(" << sorted.size() << " rows, " << std::fixed << std::setprecision(0) << ms << " ms)\n";
    std::cout << "[row store: read " << (total_bytes / 1e6) << " MB]\n";
    return true;
}
