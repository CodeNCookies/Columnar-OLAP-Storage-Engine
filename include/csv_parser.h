#pragma once
#include <string>
#include <vector>

// Parses a single CSV line into fields.
// Handles quoted fields (commas and newlines inside quotes are part of the field).
std::vector<std::string> parse_csv_line(const std::string& line);

// Holds all rows of a loaded CSV file.
struct CSVData {
    std::vector<std::string>              headers;  // column names from first row
    std::vector<std::vector<std::string>> rows;     // [row][col] = value string
};

// Load an entire CSV file from disk.
// Returns false and prints an error if the file cannot be opened.
bool load_csv(const std::string& path, CSVData& out);