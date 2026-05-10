#pragma once
#include "column_file.h"
#include "schema.h"
#include <string>
#include <vector>
#include <variant>
#include <functional>
#include <unordered_map>

// A single decoded cell value.
using Value = std::variant<int32_t, int64_t, double, std::string>;

// ── Encoding helpers ──────────────────────────────────────────────────────────

// Count distinct strings in a column. Returns the map string->id and count.
std::unordered_map<std::string, uint32_t> build_dict_map(const std::vector<Value>& values);
uint64_t count_distinct(const std::vector<Value>& values);

// Check if dictionary encoding is worth it (≤ 65535 distinct strings).
bool should_use_dict(const std::vector<Value>& values, DataType type);

// Count runs in a numeric column. Returns number of (value, run_length) pairs.
uint64_t count_runs(const std::vector<Value>& values);

// Check if RLE is worth it (runs < N/4).
bool should_use_rle(const std::vector<Value>& values, DataType type, uint64_t N);

// ── I/O ───────────────────────────────────────────────────────────────────────

// Write one column to disk in binary format.
// values must all match the type in meta.
// Returns false on I/O error.
bool write_column(const std::string& dir,
                  const ColumnMeta& meta,
                  const std::vector<Value>& values);

// Read an entire column from disk into a vector of Values.
// Returns false on error or format mismatch.
bool read_column(const std::string& dir,
                 const ColumnMeta& meta,
                 std::vector<Value>& out);

// Stream a column row-by-row without loading all values into RAM.
// callback receives (row_index, value) and returns true to continue.
bool scan_column(const std::string& dir,
                 const ColumnMeta& meta,
                 std::function<bool(uint64_t, const Value&)> callback);

// Infer a DataType from a column of raw string values.
DataType infer_type(const std::vector<std::string>& raw_values);

// Convert a raw string to a Value of the given type.
// Returns false if conversion fails.
bool parse_value(const std::string& s, DataType t, Value& out);

// Get a double representation of a numeric Value (for SUM/AVG).
double to_double(const Value& v);

// Compare two Values. Returns <0, 0, or >0.
int compare_values(const Value& a, const Value& b);

// Pretty-print a Value.
std::string value_to_string(const Value& v);