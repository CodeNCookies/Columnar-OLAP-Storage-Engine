#include "csv_parser.h"
#include "schema.h"
#include "column_rw.h"
#include <iostream>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

// Compute file size in bytes.
static uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    return ec ? 0 : sz;
}

// Load a CSV file as a named table into the warehouse directory.
// Returns false on error.
bool load_table(const std::string& csv_path,
                const std::string& table_name,
                const std::string& warehouse_dir) {
    // ── Parse CSV ─────────────────────────────────────────────────────────────
    CSVData csv;
    if (!load_csv(csv_path, csv)) return false;

    size_t num_cols = csv.headers.size();
    size_t num_rows = csv.rows.size();

    // ── Create table directory ────────────────────────────────────────────────
    std::string tbl_dir = warehouse_dir + "/" + table_name;
    fs::create_directories(tbl_dir);

    // ── Build per-column value vectors ────────────────────────────────────────
    // Transpose from row-major (csv.rows) to column-major.
    std::vector<std::vector<std::string>> raw(num_cols);
    for (size_t r = 0; r < num_rows; ++r)
        for (size_t c = 0; c < num_cols; ++c)
            raw[c].push_back(csv.rows[r][c]);

    // ── Infer types and build schema ──────────────────────────────────────────
    TableSchema schema;
    schema.table_name = table_name;

    for (size_t c = 0; c < num_cols; ++c) {
        ColumnMeta meta;
        meta.name     = csv.headers[c];
        meta.type     = infer_type(raw[c]);
        meta.encoding = Encoding::NONE;   // Phase 1: always raw
        schema.columns.push_back(meta);
    }

    // ── Write each column ─────────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << std::left
              << std::setw(20) << "column name"
              << std::setw(10) << "type"
              << std::setw(12) << "encoding"
              << "size\n";
    std::cout << std::string(52, '-') << "\n";

    uint64_t total_bytes = 0;

    for (size_t c = 0; c < num_cols; ++c) {
        const auto& meta = schema.columns[c];

        // Convert raw strings to typed Values
        std::vector<Value> values;
        values.reserve(num_rows);
        for (const auto& s : raw[c]) {
            Value v;
            if (!parse_value(s, meta.type, v)) {
                // Fallback: store as empty/zero for bad conversions
                switch (meta.type) {
                    case DataType::INT32:  v = (int32_t)0; break;
                    case DataType::INT64:  v = (int64_t)0; break;
                    case DataType::DOUBLE: v = 0.0; break;
                    case DataType::STRING: v = std::string(""); break;
                }
            }
            values.push_back(v);
        }

        if (!write_column(tbl_dir, meta, values)) return false;

        uint64_t sz = file_size(tbl_dir + "/" + meta.name + ".col");
        total_bytes += sz;
        double sz_mb = sz / 1e6;

        std::cout << std::left
                  << std::setw(20) << meta.name
                  << std::setw(10) << datatype_name(meta.type)
                  << std::setw(12) << encoding_name(meta.encoding)
                  << std::fixed << std::setprecision(1) << sz_mb << " MB\n";
    }

    // ── Write schema last (atomic) ────────────────────────────────────────────
    if (!write_schema(tbl_dir, schema)) return false;

    // ── Print summary ─────────────────────────────────────────────────────────
    uint64_t csv_size = file_size(csv_path);
    double ratio = csv_size ? (double)csv_size / total_bytes : 0.0;

    std::cout << "\nLoaded " << num_rows << " rows, " << num_cols
              << " columns into " << tbl_dir << "\n";
    std::cout << "Total on disk  : "
              << std::fixed << std::setprecision(1) << total_bytes / 1e6 << " MB\n";
    std::cout << "Original CSV   : "
              << csv_size / 1e6 << " MB\n";
    std::cout << "Compression    : " << std::setprecision(2) << ratio << "x\n";
    return true;
}