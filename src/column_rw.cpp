#include "column_rw.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <charconv>
#include <unordered_map>
#include <set>
#include <algorithm>

static std::string col_path(const std::string& dir, const std::string& name) {
    return dir + "/" + name + ".col";
}

template<typename T>
static void write_pod(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template<typename T>
static bool read_pod(std::ifstream& f, T& v) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

DataType infer_type(const std::vector<std::string>& vals) {
    bool all_int32 = true, all_int64 = true, all_double = true;
    bool has_dot = false;

    for (const auto& s : vals) {
        if (s.empty()) continue;
        if (s.find('.') != std::string::npos) has_dot = true;
        if (all_int32) {
            try { std::stoi(s); }
            catch (...) { all_int32 = false; }
        }
        if (all_int64 && !all_int32) {
            try { std::stoll(s); }
            catch (...) { all_int64 = false; }
        }
        if (all_double && !all_int64) {
            try { std::stod(s); }
            catch (...) { all_double = false; }
        }
    }

    if (has_dot && all_double) return DataType::DOUBLE;
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

std::unordered_map<std::string, uint32_t> build_dict_map(const std::vector<Value>& values) {
    std::unordered_map<std::string, uint32_t> mapping;
    uint32_t next_id = 0;
    for (const auto& v : values) {
        const std::string& s = std::get<std::string>(v);
        if (mapping.find(s) == mapping.end()) {
            mapping[s] = next_id++;
        }
    }
    return mapping;
}

uint64_t count_distinct(const std::vector<Value>& values) {
    std::set<std::string> seen;
    for (const auto& v : values) {
        seen.insert(std::get<std::string>(v));
    }
    return seen.size();
}

bool should_use_dict(const std::vector<Value>& values, DataType type) {
    if (type != DataType::STRING) return false;
    if (values.empty()) return false;
    return count_distinct(values) <= 65535;
}

uint64_t count_runs(const std::vector<Value>& values) {
    if (values.empty()) return 0;
    uint64_t runs = 1;
    for (size_t i = 1; i < values.size(); ++i) {
        if (compare_values(values[i], values[i-1]) != 0) {
            ++runs;
        }
    }
    return runs;
}

bool should_use_rle(const std::vector<Value>& values, DataType type, uint64_t N) {
    if (type == DataType::STRING) return false;
    if (N == 0) return false;
    return count_runs(values) < N / 4;
}

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

    std::ostringstream data_buf;
    std::ostringstream dict_buf;
    uint64_t dict_size = 0;

    switch (meta.encoding) {
        case Encoding::NONE: {
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
                        const std::string& s = std::get<std::string>(val);
                        uint16_t len = static_cast<uint16_t>(s.size());
                        data_buf.write(reinterpret_cast<const char*>(&len), 2);
                        data_buf.write(s.data(), len);
                        break;
                    }
                }
            }
            break;
        }

        case Encoding::DICT: {
            auto mapping = build_dict_map(values);
            uint32_t D = mapping.size();
            bool use_1byte = (D <= 256);

            for (const auto& val : values) {
                const std::string& s = std::get<std::string>(val);
                uint32_t id = mapping[s];
                if (use_1byte) {
                    uint8_t b = static_cast<uint8_t>(id);
                    data_buf.write(reinterpret_cast<const char*>(&b), 1);
                } else {
                    uint16_t w = static_cast<uint16_t>(id);
                    data_buf.write(reinterpret_cast<const char*>(&w), 2);
                }
            }

            uint32_t dict_count = D;
            dict_buf.write(reinterpret_cast<const char*>(&dict_count), 4);

            std::vector<std::string> reverse(D);
            for (const auto& [str, id] : mapping) {
                reverse[id] = str;
            }
            for (uint32_t id = 0; id < D; ++id) {
                uint16_t id16 = static_cast<uint16_t>(id);
                const std::string& s = reverse[id];
                uint16_t len = static_cast<uint16_t>(s.size());
                dict_buf.write(reinterpret_cast<const char*>(&id16), 2);
                dict_buf.write(reinterpret_cast<const char*>(&len), 2);
                dict_buf.write(s.data(), len);
            }
            dict_size = dict_buf.tellp();
            break;
        }

        case Encoding::RLE: {
            for (size_t i = 0; i < values.size(); ) {
                size_t j = i + 1;
                while (j < values.size() && compare_values(values[j], values[i]) == 0) {
                    ++j;
                }
                uint32_t run_count = static_cast<uint32_t>(j - i);

                const auto& val = values[i];
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
                    default: break;
                }

                data_buf.write(reinterpret_cast<const char*>(&run_count), 4);
                i = j;
            }
            break;
        }
    }

    std::string data_str = data_buf.str();

    ColHeader hdr{};
    std::memcpy(hdr.magic, COL_MAGIC, 4);
    hdr.version   = COL_VERSION;
    hdr.data_type = static_cast<uint8_t>(meta.type);
    hdr.encoding  = static_cast<uint8_t>(meta.encoding);
    hdr.reserved  = 0;
    hdr.row_count = values.size();
    hdr.data_size = data_str.size();
    hdr.dict_size = dict_size;

    write_pod(f, hdr);
    f.write(data_str.data(), data_str.size());
    if (dict_size > 0) {
        std::string dict_str = dict_buf.str();
        f.write(dict_str.data(), dict_str.size());
    }
    f.close();

    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::cerr << "Cannot rename: " << tmp << "\n";
        return false;
    }
    return true;
}

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

    if (std::memcmp(hdr.magic, COL_MAGIC, 4) != 0 || hdr.version != COL_VERSION) {
        std::cerr << "Bad magic/version in: " << path << "\n";
        return false;
    }

    Encoding enc = static_cast<Encoding>(hdr.encoding);
    DataType dtype = static_cast<DataType>(hdr.data_type);

    out.reserve(hdr.row_count);

    std::vector<std::string> dict_reverse;
    if (enc == Encoding::DICT) {
        f.seekg(sizeof(ColHeader) + hdr.data_size);
        uint32_t dict_count;
        f.read(reinterpret_cast<char*>(&dict_count), 4);
        dict_reverse.resize(dict_count);
        for (uint32_t i = 0; i < dict_count; ++i) {
            uint16_t id16;
            uint16_t len;
            f.read(reinterpret_cast<char*>(&id16), 2);
            f.read(reinterpret_cast<char*>(&len), 2);
            std::string s(len, '\0');
            f.read(s.data(), len);
            dict_reverse[id16] = std::move(s);
        }
        f.seekg(sizeof(ColHeader));
    }

    uint32_t dict_count = static_cast<uint32_t>(dict_reverse.size());
    bool use_1byte = (dict_count <= 256);

    if (enc != Encoding::RLE) {
        for (uint64_t i = 0; i < hdr.row_count; ++i) {
            Value v;
            switch (enc) {
                case Encoding::NONE: {
                    switch (dtype) {
                        case DataType::INT32:  { int32_t x;  read_pod(f, x); v = x; break; }
                        case DataType::INT64:  { int64_t x;  read_pod(f, x); v = x; break; }
                        case DataType::DOUBLE: { double x;   read_pod(f, x); v = x; break; }
                        case DataType::STRING: {
                            uint16_t len; read_pod(f, len);
                            std::string s(len, '\0');
                            f.read(s.data(), len);
                            v = std::move(s);
                            break;
                        }
                    }
                    break;
                }
                case Encoding::DICT: {
                    uint32_t id = 0;
                    if (use_1byte) {
                        uint8_t b; read_pod(f, b); id = b;
                    } else {
                        uint16_t w; read_pod(f, w); id = w;
                    }
                    v = dict_reverse[id];
                    break;
                }
                default: break;
            }
            out.push_back(v);
        }
    }

    if (enc == Encoding::RLE) {
        f.seekg(sizeof(ColHeader));
        uint64_t rows_read = 0;
        while (rows_read < hdr.row_count) {
            Value v;
            switch (dtype) {
                case DataType::INT32:  { int32_t x;  read_pod(f, x); v = x; break; }
                case DataType::INT64:  { int64_t x;  read_pod(f, x); v = x; break; }
                case DataType::DOUBLE: { double x;   read_pod(f, x); v = x; break; }
                default: break;
            }
            uint32_t run_count;
            read_pod(f, run_count);
            for (uint32_t r = 0; r < run_count && rows_read < hdr.row_count; ++r) {
                out.push_back(v);
                ++rows_read;
            }
        }
    }

    return true;
}

bool scan_column(const std::string& dir,
                 const ColumnMeta& meta,
                 std::function<bool(uint64_t, const Value&)> callback) {
    std::vector<Value> vals;
    if (!read_column(dir, meta, vals)) return false;
    for (uint64_t i = 0; i < vals.size(); ++i) {
        if (!callback(i, vals[i])) break;
    }
    return true;
}
