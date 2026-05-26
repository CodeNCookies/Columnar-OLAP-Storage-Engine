#pragma once
#include "column_file.h"
#include "column_rw.h"
#include <string>
#include <vector>
#include <functional>

// ── Row-oriented storage (Phase 3 baseline) ──────────────────────────────────

// RowStore is deliberately simple: one binary file per table.
// Format: [row_count:8 bytes] then for each row:
//   [col_count:2 bytes] then for each column:
//     [type:1 byte][len:4 bytes][value_bytes...]
// No compression, no indexing, no block structure.

// Column descriptor for row store (simpler than ColumnMeta)
struct RowColInfo {
    std::string name;
    DataType    type;
};

struct RowTableInfo {
    std::string              table_name;
    std::vector<RowColInfo>  columns;
    uint64_t                 row_count;
};

// Load a CSV and write it as a row-store file.
// Returns false on error.
bool row_load_csv(const std::string& csv_path,
                  const std::string& table_name,
                  const std::string& warehouse_dir);

// Read the row-store file header (column info + row count).
bool row_read_info(const std::string& dir,
                   const std::string& table_name,
                   RowTableInfo& info);

// Execute a query against the row store.
// Uses the same Query struct from query.h.
// Prints results to stdout. Returns false on error.
bool row_execute_query(const struct Query& q, const std::string& warehouse_dir);
