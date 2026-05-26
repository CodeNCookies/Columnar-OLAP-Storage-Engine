#include "csv_parser.h"
#include "schema.h"
#include "column_rw.h"
#include "query.h"
#include "row_store.h"
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>

// Declared in loader.cpp
bool load_table(const std::string& csv_path,
                const std::string& table_name,
                const std::string& warehouse_dir);

// Run a single query against both stores and print timings
static void bench_query(const Query& q, const std::string& warehouse_dir,
                        const std::string& label) {
    namespace ch = std::chrono;
    std::cout << "\n  " << label << ":\n";

    // Column store
    {
        auto t0 = ch::steady_clock::now();
        std::cout << "    column store: ";
        execute_query(q, warehouse_dir);
        auto t1 = ch::steady_clock::now();
        double ms = ch::duration<double, std::milli>(t1 - t0).count();
        std::cout << "    (column store time: " << std::fixed << std::setprecision(0) << ms << " ms)\n";
    }

    // Row store
    {
        auto t0 = ch::steady_clock::now();
        std::cout << "    row store: ";
        row_execute_query(q, warehouse_dir);
        auto t1 = ch::steady_clock::now();
        double ms = ch::duration<double, std::milli>(t1 - t0).count();
        std::cout << "    (row store time: " << std::fixed << std::setprecision(0) << ms << " ms)\n";
    }
}

int main(int argc, char* argv[]) {
    std::string warehouse = "./warehouse";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc)
            warehouse = argv[++i];
    }

    std::filesystem::create_directories(warehouse);

    std::cout << "colsh > using warehouse: " << warehouse << "\n";
    std::cout << "Commands: LOAD <file.csv> AS <table>  |  QUERY SELECT ...  |  "
                 "ROWLOAD <file.csv> AS <table>  |  ROWQUERY SELECT ...  |  "
                 "BENCH run  |  \\quit\n\n";

    std::string line;
    while (true) {
        std::cout << "colsh > ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;

        size_t s = line.find_first_not_of(" \t\r\n");
        size_t e = line.find_last_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        line = line.substr(s, e - s + 1);

        if (line.empty()) continue;
        if (line == "\\quit" || line == "quit" || line == "exit") break;

        // ── LOAD <csv> AS <table> (column store) ──────────────────────────
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

        // ── ROWLOAD <csv> AS <table> (row store) ──────────────────────────
        if (line.substr(0, 7) == "ROWLOAD") {
            std::istringstream ss(line.substr(7));
            std::string csv_path, as_kw, table_name;
            ss >> csv_path >> as_kw >> table_name;

            if (as_kw != "AS" || csv_path.empty() || table_name.empty()) {
                std::cout << "Usage: ROWLOAD <file.csv> AS <table_name>\n";
                continue;
            }
            row_load_csv(csv_path, table_name, warehouse);
            continue;
        }

        // ── QUERY <sql> (column store) ────────────────────────────────────
        if (line.substr(0, 5) == "QUERY") {
            std::string sql = line.substr(5);
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

        // ── ROWQUERY <sql> (row store) ────────────────────────────────────
        if (line.substr(0, 8) == "ROWQUERY") {
            std::string sql = line.substr(8);
            size_t p = sql.find_first_not_of(" \t");
            if (p != std::string::npos) sql = sql.substr(p);

            Query q;
            std::string err;
            if (!parse_query(sql, q, err)) {
                std::cout << "Parse error: " << err << "\n";
                continue;
            }
            row_execute_query(q, warehouse);
            continue;
        }

        // ── BENCH run ─────────────────────────────────────────────────────
        if (line == "BENCH run") {
            std::cout << "\n=== Benchmark: Column Store vs Row Store ===\n";

            // Query 1: SUM with WHERE (2-column analytical)
            {
                Query q;
                std::string err;
                parse_query("SELECT SUM(price) FROM sales WHERE date >= 20240101", q, err);
                bench_query(q, warehouse, "Query 1: SELECT SUM(price) FROM sales WHERE date >= 20240101");
            }

            // Query 2: GROUP BY aggregate (2-column)
            {
                Query q;
                std::string err;
                parse_query("SELECT country, AVG(quantity) FROM sales GROUP BY country", q, err);
                bench_query(q, warehouse, "Query 2: SELECT country, AVG(quantity) FROM sales GROUP BY country");
            }

            // Query 3: Point lookup (all columns)
            {
                Query q;
                std::string err;
                parse_query("SELECT * FROM sales WHERE id = 500000", q, err);
                bench_query(q, warehouse, "Query 3: SELECT * FROM sales WHERE id = 500000");
            }

            std::cout << "\n=== Benchmark complete ===\n";
            continue;
        }

        std::cout << "Unknown command. Try LOAD, QUERY, ROWLOAD, ROWQUERY, BENCH, or \\quit\n";
    }

    std::cout << "Bye.\n";
    return 0;
}
