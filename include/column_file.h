#pragma once
#include <cstdint>
#include <string>

// Magic bytes for column file identification
constexpr uint8_t COL_MAGIC[4] = {'C', 'O', 'L', '1'};
constexpr uint32_t COL_VERSION = 1;

// Data type codes stored in column file header
enum class DataType : uint8_t {
    INT32  = 1,
    INT64  = 2,
    DOUBLE = 3,
    STRING = 4
};

// Encoding codes stored in column file header
enum class Encoding : uint8_t {
    NONE = 0,  // raw binary
    DICT = 1,  // dictionary encoding (for strings)
    RLE  = 2   // run-length encoding (for numerics)
};

// On-disk column file header (36 bytes total, packed)
#pragma pack(push, 1)
struct ColHeader {
    uint8_t  magic[4];     // 'COL1'
    uint32_t version;      // must be 1
    uint8_t  data_type;    // DataType enum
    uint8_t  encoding;     // Encoding enum
    uint16_t reserved;     // must be 0
    uint64_t row_count;    // total rows
    uint64_t data_size;    // bytes in data section
    uint64_t dict_size;    // bytes in dict section (0 if N/A)
};
#pragma pack(pop)

static_assert(sizeof(ColHeader) == 36, "ColHeader must be 36 bytes");

// Human-readable names for display
inline std::string datatype_name(DataType t) {
    switch (t) {
        case DataType::INT32:  return "int32";
        case DataType::INT64:  return "int64";
        case DataType::DOUBLE: return "double";
        case DataType::STRING: return "string";
        default:               return "unknown";
    }
}

inline std::string encoding_name(Encoding e) {
    switch (e) {
        case Encoding::NONE: return "none";
        case Encoding::DICT: return "dict";
        case Encoding::RLE:  return "rle";
        default:             return "unknown";
    }
}