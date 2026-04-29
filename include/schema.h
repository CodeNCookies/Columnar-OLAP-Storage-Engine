#pragma once
#include "column_file.h"
#include <string>
#include <vector>

// Describes one column inside a loaded table.
struct ColumnMeta {
    std::string name;
    DataType    type;
    Encoding    encoding;
};

// Describes a loaded table (collection of columns on disk).
struct TableSchema {
    std::string              table_name;
    std::vector<ColumnMeta>  columns;
};

// Write schema.json for a table into the given directory.
// Uses temp-file-and-rename so a crash mid-write is safe.
bool write_schema(const std::string& dir, const TableSchema& schema);

// Read schema.json from the given directory.
bool read_schema(const std::string& dir, TableSchema& schema);

// Return index of a column by name, or -1 if not found.
int find_column(const TableSchema& schema, const std::string& name);