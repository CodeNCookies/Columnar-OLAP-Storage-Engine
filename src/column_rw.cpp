#include "column_rw.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <charconv>

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string col_path(const std::string& dir, const std::string& name) {
    return dir + "/" + name + ".col";
}

// Write raw bytes from a trivially-copyable value.
template<typename T>
static void write_pod(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

// Read raw bytes into a trivially-copyable value.
template<typename T>
static bool read_pod(std::ifstream& f, T& v) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

// ── Type inference ────────────────────────────────────────────────────────────

DataType infer_type(const std::vector<std::string>& vals) {
    bool all_int32 = true, all_int64 = true, all_double = true;

    for (const auto& s : vals) {
        if (s.empty()) continue;
        // Try int32
        if (all_int32) {
            try { int32_t v; std::stoi(s); (void)v; }
            catch (...) { all_int32 = false; }
        }
        // Try int64
        if (all_int64 && !all_int32) {
            try { std::stoll(s); }
            catch (...) { all_int64 = false; }
        }
        // Try double
        if (all_double && !all_int64) {
            try { std::stod(s); }
            catch (...) { all_double = false; }
        }
    }

    if (all_int32)  return DataType::INT32;
    if (all_int64)  return DataType::INT64;
    if (all_double) return DataType::DOUBLE;
    return DataType::STRING;
}

bool parse_value(const std::string& s, DataType t, Value& out) {
    try {
        switch (t) {
            case DataType::INT32:  out = (int32_t)std::stoi(s);  return true;
            case DataType::INT64:  out = (int64_t)std::stoll(s); return true;
            case DataType::DOUBLE: out = std::stod(s);           return true;
            case DataType::STRING: out = s;                       return true;
        }
    } catch (...) {}
    return false;
}

double to_double(const Value& v) {
    return std::visit([](auto&& x) -> double {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_arithmetic_v<T>) return static_cast<double>(x);
        return 0.0;
    }, v);
}

int compare_values(const Value& a, const Value& b) {
    // Both must be the same type (guaranteed by schema).
    // Both are the same type (guaranteed by schema).
    if (a.index() != b.index()) return 0;
    return std::visit([&b](auto&& x) -> int {
        using T = std::decay_t<decltype(x)>;
        const T& y = std::get<T>(b);
        if (x < y) return -1;
        if (x > y) return  1;
        return 0;
    }, a);
}

std::string value_to_string(const Value& v) {
    return std::visit([](auto&& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return x;
        else return std::to_string(x);
    }, v);
}

// ── Writer ────────────────────────────────────────────────────────────────────
// Phase 1: only NONE (raw) encoding.

bool write_column(const std::string& dir,
                  const ColumnMeta& meta,
                  const std::vector<Value>& values) {
    std::string tmp  = col_path(dir, meta.name) + ".tmp";
    std::string dest = col_path(dir, meta.name);

    std::ofstream f(tmp, std::ios::binary);
    if (!f) {
        std::cerr << "Cannot write: " << tmp << "\n";
        return false;
    }

    // Build data section in memory first so we know data_size.
    std::ostringstream data_buf;
    for (const auto& val : values) {
        switch (meta.type) {
            case DataType::INT32: {
                int32_t v = std::get<int32_t>(val);
                data_buf.write(reinterpret_cast<const char*>(&v), 4);
                break;
            }
            case DataType::INT64: {
                int64_t v = std::get<int64_t>(val);
                data_buf.write(reinterpret_cast<const char*>(&v), 8);
                break;
            }
            case DataType::DOUBLE: {
                double v = std::get<double>(val);
                data_buf.write(reinterpret_cast<const char*>(&v), 8);
                break;
            }
            case DataType::STRING: {
                // 2-byte length prefix + bytes
                const std::string& s = std::get<std::string>(val);
                uint16_t len = static_cast<uint16_t>(s.size());
                data_buf.write(reinterpret_cast<const char*>(&len), 2);
                data_buf.write(s.data(), len);
                break;
            }
        }
    }

    std::string data_str = data_buf.str();

    // Write header
    ColHeader hdr{};
    std::memcpy(hdr.magic, COL_MAGIC, 4);
    hdr.version   = COL_VERSION;
    hdr.data_type = static_cast<uint8_t>(meta.type);
    hdr.encoding  = static_cast<uint8_t>(meta.encoding);
    hdr.reserved  = 0;
    hdr.row_count = values.size();
    hdr.data_size = data_str.size();
    hdr.dict_size = 0;

    write_pod(f, hdr);
    f.write(data_str.data(), data_str.size());
    f.close();

    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::cerr << "Cannot rename: " << tmp << "\n";
        return false;
    }
    return true;
}

// ── Reader (full load) ────────────────────────────────────────────────────────

bool read_column(const std::string& dir,
                 const ColumnMeta& meta,
                 std::vector<Value>& out) {
    std::string path = col_path(dir, meta.name);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Cannot open: " << path << "\n";
        return false;
    }

    ColHeader hdr{};
    if (!read_pod(f, hdr)) return false;

    // Validate magic and version
    if (std::memcmp(hdr.magic, COL_MAGIC, 4) != 0 || hdr.version != COL_VERSION) {
        std::cerr << "Bad magic/version in: " << path << "\n";
        return false;
    }

    out.reserve(hdr.row_count);

    for (uint64_t i = 0; i < hdr.row_count; ++i) {
        Value v;
        switch (static_cast<DataType>(hdr.data_type)) {
            case DataType::INT32: {
                int32_t x; read_pod(f, x); v = x; break;
            }
            case DataType::INT64: {
                int64_t x; read_pod(f, x); v = x; break;
            }
            case DataType::DOUBLE: {
                double x; read_pod(f, x); v = x; break;
            }
            case DataType::STRING: {
                uint16_t len; read_pod(f, len);
                std::string s(len, '\0');
                f.read(s.data(), len);
                v = std::move(s);
                break;
            }
        }
        out.push_back(v);
    }
    return true;
}

// ── Scanner (streaming callback) ─────────────────────────────────────────────
// Identical to read_column but calls callback per-row to avoid full RAM load.

bool scan_column(const std::string& dir,
                 const ColumnMeta& meta,
                 std::function<bool(uint64_t, const Value&)> callback) {
    std::string path = col_path(dir, meta.name);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Cannot open: " << path << "\n";
        return false;
    }

    ColHeader hdr{};
    if (!read_pod(f, hdr)) return false;

    if (std::memcmp(hdr.magic, COL_MAGIC, 4) != 0 || hdr.version != COL_VERSION) {
        std::cerr << "Bad magic/version in: " << path << "\n";
        return false;
    }

    for (uint64_t i = 0; i < hdr.row_count; ++i) {
        Value v;
        switch (static_cast<DataType>(hdr.data_type)) {
            case DataType::INT32: {
                int32_t x; read_pod(f, x); v = x; break;
            }
            case DataType::INT64: {
                int64_t x; read_pod(f, x); v = x; break;
            }
            case DataType::DOUBLE: {
                double x; read_pod(f, x); v = x; break;
            }
            case DataType::STRING: {
                uint16_t len; read_pod(f, len);
                std::string s(len, '\0');
                f.read(s.data(), len);
                v = std::move(s);
                break;
            }
        }
        if (!callback(i, v)) break;
    }
    return true;
}