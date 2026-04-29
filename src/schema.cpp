#include "schema.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>   // rename, tmpnam

// ── Minimal JSON writer ───────────────────────────────────────────────────────
// We write a fixed JSON structure; no need for a library.

bool write_schema(const std::string& dir, const TableSchema& schema) {
    std::string tmp  = dir + "/schema.json.tmp";
    std::string dest = dir + "/schema.json";

    std::ofstream f(tmp);
    if (!f) {
        std::cerr << "Cannot write schema to: " << tmp << "\n";
        return false;
    }

    f << "{\n";
    f << "  \"table\": \"" << schema.table_name << "\",\n";
    f << "  \"columns\": [\n";

    for (size_t i = 0; i < schema.columns.size(); ++i) {
        const auto& c = schema.columns[i];
        f << "    { \"name\": \"" << c.name
          << "\", \"type\": " << static_cast<int>(c.type)
          << ", \"encoding\": " << static_cast<int>(c.encoding) << " }";
        if (i + 1 < schema.columns.size()) f << ",";
        f << "\n";
    }

    f << "  ]\n}\n";
    f.close();

    // Atomic rename: if this crashes mid-write, the .tmp file is left behind
    // but the original schema.json is untouched.
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::cerr << "Cannot rename schema file\n";
        return false;
    }
    return true;
}

// ── Minimal JSON reader ───────────────────────────────────────────────────────
// Hand-written because we control the exact format we wrote above.

static std::string extract_str(const std::string& src, const std::string& key) {
    // Find  "key": "VALUE"  and return VALUE
    std::string pat = "\"" + key + "\": \"";
    auto pos = src.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    auto end = src.find('"', pos);
    return src.substr(pos, end - pos);
}

static int extract_int(const std::string& src, const std::string& key) {
    std::string pat = "\"" + key + "\": ";
    auto pos = src.find(pat);
    if (pos == std::string::npos) return -1;
    pos += pat.size();
    return std::stoi(src.substr(pos));
}

bool read_schema(const std::string& dir, TableSchema& schema) {
    std::string path = dir + "/schema.json";
    std::ifstream f(path);
    if (!f) {
        std::cerr << "No schema at: " << path << "\n";
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    schema.table_name = extract_str(src, "table");
    schema.columns.clear();

    // Each column is on its own line:  { "name": "...", "type": N, "encoding": M }
    size_t pos = 0;
    while ((pos = src.find("{ \"name\":", pos)) != std::string::npos) {
        size_t end = src.find("}", pos);
        std::string chunk = src.substr(pos, end - pos + 1);

        ColumnMeta cm;
        cm.name     = extract_str(chunk, "name");
        cm.type     = static_cast<DataType>(extract_int(chunk, "type"));
        cm.encoding = static_cast<Encoding>(extract_int(chunk, "encoding"));
        schema.columns.push_back(cm);
        pos = end + 1;
    }

    return !schema.columns.empty();
}

int find_column(const TableSchema& schema, const std::string& name) {
    for (int i = 0; i < (int)schema.columns.size(); ++i)
        if (schema.columns[i].name == name) return i;
    return -1;
}