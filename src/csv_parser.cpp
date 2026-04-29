#include "csv_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>

// Parse one CSV line respecting double-quote escaping.
// A quoted field may contain commas and escaped quotes ("").
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (in_quotes) {
            if (c == '"') {
                // Peek ahead: "" inside quotes is an escaped quote
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    in_quotes = false;  // closing quote
                }
            } else {
                field += c;
            }
        } else {
            if (c == '"') {
                in_quotes = true;
            } else if (c == ',') {
                fields.push_back(field);
                field.clear();
            } else if (c == '\r') {
                // strip carriage return for Windows-style line endings
            } else {
                field += c;
            }
        }
    }
    fields.push_back(field);  // last field (no trailing comma)
    return fields;
}

bool load_csv(const std::string& path, CSVData& out) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Cannot open CSV: " << path << "\n";
        return false;
    }

    std::string line;

    // First line = header
    if (!std::getline(f, line)) {
        std::cerr << "CSV is empty: " << path << "\n";
        return false;
    }
    out.headers = parse_csv_line(line);

    // Remaining lines = data rows
    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;  // skip blank lines
        auto row = parse_csv_line(line);
        // Pad short rows with empty strings so every row has the same width
        while (row.size() < out.headers.size()) row.push_back("");
        out.rows.push_back(std::move(row));
    }
    return true;
}