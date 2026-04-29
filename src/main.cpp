#include "csv_parser.h"
#include "schema.h"
#include "column_rw.h"
#include "query.h"
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>

// Declared in loader.cpp
bool load_table(const std::string& csv_path,
                const std::string& table_name,
                const std::string& warehouse_dir);

int main(int argc, char* argv[]) {
    // Default warehouse directory; overridden by --data flag.
    std::string warehouse = "./warehouse";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc)
            warehouse = argv[++i];
    }

    std::filesystem::create_directories(warehouse);

    std::cout << "colsh > using warehouse: " << warehouse << "\n";
    std::cout << "Commands: LOAD <file.csv> AS <table>  |  "
                 "QUERY SELECT ...  |  \\quit\n\n";

    std::string line;
    while (true) {
        std::cout << "colsh > ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;  // EOF (Ctrl-D)

        // Trim leading/trailing whitespace
        size_t s = line.find_first_not_of(" \t\r\n");
        size_t e = line.find_last_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        line = line.substr(s, e - s + 1);

        if (line.empty()) continue;
        if (line == "\\quit" || line == "quit" || line == "exit") break;

        // ── LOAD <csv> AS <table> ─────────────────────────────────────────────
        if (line.substr(0, 4) == "LOAD") {
            std::istringstream ss(line.substr(4));
            std::string csv_path, as_kw, table_name;
            ss >> csv_path >> as_kw >> table_name;

            if (as_kw != "AS" || csv_path.empty() || table_name.empty()) {
                std::cout << "Usage: LOAD <file.csv> AS <table_name>\n";
                continue;
            }
            load_table(csv_path, table_name, warehouse);
            continue;
        }

        // ── QUERY <sql> ───────────────────────────────────────────────────────
        if (line.substr(0, 5) == "QUERY") {
            std::string sql = line.substr(5);
            // Trim
            size_t p = sql.find_first_not_of(" \t");
            if (p != std::string::npos) sql = sql.substr(p);

            Query q;
            std::string err;
            if (!parse_query(sql, q, err)) {
                std::cout << "Parse error: " << err << "\n";
                continue;
            }
            execute_query(q, warehouse);
            continue;
        }

        std::cout << "Unknown command. Try LOAD, QUERY, or \\quit\n";
    }

    std::cout << "Bye.\n";
    return 0;
}