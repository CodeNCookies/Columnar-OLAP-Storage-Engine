#include "csv_parser.h"
#include "schema.h"
#include "column_rw.h"
#include "query.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Declared in loader.cpp
bool load_table(const std::string& csv_path,
                const std::string& table_name,
                const std::string& warehouse_dir);

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; std::cout << "  " << name << "... "; } while(0)
#define PASS() do { tests_passed++; std::cout << "PASSED\n"; } while(0)
#define FAIL(msg) do { std::cout << "FAILED: " << msg << "\n"; return; } while(0)

// ── Helper: create a small CSV file ──────────────────────────────────────────
static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
    f.close();
}

// ── Test 1: Dictionary encoding round-trip ───────────────────────────────────
static void test_dict_roundtrip() {
    TEST("Dictionary encoding round-trip");
    fs::create_directories("/tmp/colap_test");

    ColumnMeta meta;
    meta.name = "country";
    meta.type = DataType::STRING;
    meta.encoding = Encoding::DICT;

    std::vector<Value> input;
    input.push_back(std::string("Germany"));
    input.push_back(std::string("USA"));
    input.push_back(std::string("Germany"));
    input.push_back(std::string("UK"));
    input.push_back(std::string("USA"));
    input.push_back(std::string("Germany"));

    if (!write_column("/tmp/colap_test", meta, input))
        FAIL("write_column failed");

    std::vector<Value> output;
    if (!read_column("/tmp/colap_test", meta, output))
        FAIL("read_column failed");

    if (output.size() != input.size())
        FAIL("size mismatch: " + std::to_string(output.size()) + " vs " + std::to_string(input.size()));

    for (size_t i = 0; i < input.size(); ++i) {
        if (std::get<std::string>(output[i]) != std::get<std::string>(input[i]))
            FAIL("value mismatch at row " + std::to_string(i) + ": " +
                 std::get<std::string>(output[i]) + " vs " + std::get<std::string>(input[i]));
    }

    fs::remove_all("/tmp/colap_test");
    PASS();
}

// ── Test 2: RLE encoding round-trip ──────────────────────────────────────────
static void test_rle_roundtrip() {
    TEST("RLE encoding round-trip");
    fs::create_directories("/tmp/colap_test");

    ColumnMeta meta;
    meta.name = "date";
    meta.type = DataType::INT32;
    meta.encoding = Encoding::RLE;

    std::vector<Value> input;
    // 3 runs: 10, 10, 10, 20, 20, 30, 30, 30, 30
    for (int i = 0; i < 3; ++i)  input.push_back((int32_t)10);
    for (int i = 0; i < 2; ++i)  input.push_back((int32_t)20);
    for (int i = 0; i < 4; ++i)  input.push_back((int32_t)30);

    if (!write_column("/tmp/colap_test", meta, input))
        FAIL("write_column failed");

    std::vector<Value> output;
    if (!read_column("/tmp/colap_test", meta, output))
        FAIL("read_column failed");

    if (output.size() != input.size())
        FAIL("size mismatch: " + std::to_string(output.size()) + " vs " + std::to_string(input.size()));

    for (size_t i = 0; i < input.size(); ++i) {
        if (std::get<int32_t>(output[i]) != std::get<int32_t>(input[i]))
            FAIL("value mismatch at row " + std::to_string(i));
    }

    fs::remove_all("/tmp/colap_test");
    PASS();
}

// ── Test 3: Encoding selector picks DICT for strings ─────────────────────────
static void test_selector_dict() {
    TEST("Encoding selector picks DICT for strings");
    std::vector<Value> vals;
    vals.push_back(std::string("apple"));
    vals.push_back(std::string("banana"));
    vals.push_back(std::string("apple"));
    vals.push_back(std::string("cherry"));
    vals.push_back(std::string("banana"));
    vals.push_back(std::string("apple"));

    if (!should_use_dict(vals, DataType::STRING))
        FAIL("should_use_dict returned false for string column with 3 distinct values");
    PASS();
}

// ── Test 4: Encoding selector skips DICT for numerics ────────────────────────
static void test_selector_no_dict_for_int() {
    TEST("Encoding selector skips DICT for ints");
    std::vector<Value> vals;
    vals.push_back((int32_t)1);
    vals.push_back((int32_t)2);
    vals.push_back((int32_t)1);

    if (should_use_dict(vals, DataType::INT32))
        FAIL("should_use_dict returned true for int column");
    PASS();
}

// ── Test 5: Encoding selector picks RLE for runs ─────────────────────────────
static void test_selector_rle() {
    TEST("Encoding selector picks RLE for long runs");
    std::vector<Value> vals;
    // 100 values, only 4 runs => runs < N/4 (4 < 25) => RLE
    for (int i = 0; i < 25; ++i) vals.push_back((int32_t)10);
    for (int i = 0; i < 25; ++i) vals.push_back((int32_t)20);
    for (int i = 0; i < 25; ++i) vals.push_back((int32_t)30);
    for (int i = 0; i < 25; ++i) vals.push_back((int32_t)40);

    if (!should_use_rle(vals, DataType::INT32, vals.size()))
        FAIL("should_use_rle returned false (4 runs, 100 rows)");
    PASS();
}

// ── Test 6: Encoding selector skips RLE for high cardinality ─────────────────
static void test_selector_no_rle() {
    TEST("Encoding selector skips RLE for high cardinality");
    std::vector<Value> vals;
    // 100 values, 100 runs => runs > N/4 => no RLE
    for (int i = 0; i < 100; ++i) vals.push_back((int32_t)i);

    if (should_use_rle(vals, DataType::INT32, vals.size()))
        FAIL("should_use_rle returned true (100 runs, 100 rows)");
    PASS();
}

// ── Test 7: GROUP BY query ───────────────────────────────────────────────────
static void test_group_by_query() {
    TEST("GROUP BY query execution");
    fs::create_directories("/tmp/colap_e2e_warehouse");

    // Create CSV
    write_file("/tmp/colap_test_sales.csv",
        "id,date,country,category,quantity,price\n"
        "1,20240101,Germany,Books,2,10.00\n"
        "2,20240102,USA,Electronics,1,50.00\n"
        "3,20240103,Germany,Books,3,15.00\n"
        "4,20240104,UK,Clothing,1,30.00\n"
        "5,20240105,USA,Electronics,2,40.00\n");

    if (!load_table("/tmp/colap_test_sales.csv", "sales", "/tmp/colap_e2e_warehouse"))
        FAIL("load_table failed");

    // Query: SELECT country, SUM(price) FROM sales GROUP BY country
    Query q;
    std::string err;

    if (!parse_query("SELECT country, SUM(price) FROM sales GROUP BY country", q, err))
        FAIL("parse failed: " + err);

    // Redirect stdout to capture output
    std::stringstream capture;
    auto* old_buf = std::cout.rdbuf(capture.rdbuf());

    if (!execute_query(q, "/tmp/colap_e2e_warehouse"))
        FAIL("execute_query failed");

    std::cout.rdbuf(old_buf);

    std::string output = capture.str();
    // Check that expected values appear
    if (output.find("Germany") == std::string::npos) FAIL("output missing Germany");
    if (output.find("USA") == std::string::npos)      FAIL("output missing USA");
    if (output.find("UK") == std::string::npos)        FAIL("output missing UK");
    if (output.find("25.00") == std::string::npos)     FAIL("output missing Germany sum 25.00");
    if (output.find("90.00") == std::string::npos)     FAIL("output missing USA sum 90.00");

    fs::remove_all("/tmp/colap_e2e_warehouse");
    fs::remove("/tmp/colap_test_sales.csv");
    PASS();
}

// ── Test 8: COUNT(*) with WHERE ──────────────────────────────────────────────
static void test_count_star() {
    TEST("COUNT(*) with WHERE");
    fs::create_directories("/tmp/colap_e2e_warehouse");

    write_file("/tmp/colap_test_sales.csv",
        "id,date,country,category,quantity,price\n"
        "1,20240101,Germany,Books,2,10.00\n"
        "2,20240102,USA,Electronics,1,50.00\n"
        "3,20240103,Germany,Books,3,15.00\n"
        "4,20240104,UK,Clothing,1,30.00\n"
        "5,20240105,USA,Electronics,2,40.00\n");

    if (!load_table("/tmp/colap_test_sales.csv", "sales", "/tmp/colap_e2e_warehouse"))
        FAIL("load failed");

    Query q;
    std::string err;
    if (!parse_query("SELECT COUNT(*) FROM sales WHERE country = 'Germany'", q, err))
        FAIL("parse failed: " + err);

    std::stringstream capture;
    auto* old_buf = std::cout.rdbuf(capture.rdbuf());
    if (!execute_query(q, "/tmp/colap_e2e_warehouse")) FAIL("execute failed");
    std::cout.rdbuf(old_buf);

    std::string output = capture.str();
    if (output.find("2") == std::string::npos) FAIL("expected count 2, got: " + output);

    fs::remove_all("/tmp/colap_e2e_warehouse");
    fs::remove("/tmp/colap_test_sales.csv");
    PASS();
}

// ── Test 9: Verify encoding appears in schema after load ─────────────────────
static void test_schema_encodings() {
    TEST("Schema stores encoding info after load");
    fs::create_directories("/tmp/colap_e2e_warehouse");

    write_file("/tmp/colap_test_sales.csv",
        "id,date,country,category,quantity,price\n"
        "1,20240101,Germany,Books,2,10.00\n"
        "2,20240102,USA,Electronics,1,50.00\n"
        "3,20240103,Germany,Books,3,15.00\n");

    if (!load_table("/tmp/colap_test_sales.csv", "sales", "/tmp/colap_e2e_warehouse"))
        FAIL("load failed");

    TableSchema schema;
    if (!read_schema("/tmp/colap_e2e_warehouse/sales", schema))
        FAIL("read_schema failed");

    // country and category should be DICT
    for (const auto& col : schema.columns) {
        if (col.name == "country" && col.encoding != Encoding::DICT)
            FAIL("country should be DICT but is " + encoding_name(col.encoding));
        if (col.name == "category" && col.encoding != Encoding::DICT)
            FAIL("category should be DICT but is " + encoding_name(col.encoding));
    }

    fs::remove_all("/tmp/colap_e2e_warehouse");
    fs::remove("/tmp/colap_test_sales.csv");
    PASS();
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== Phase 2 Tests ===\n\n";

    // Unit tests
    test_dict_roundtrip();
    test_rle_roundtrip();
    test_selector_dict();
    test_selector_no_dict_for_int();
    test_selector_rle();
    test_selector_no_rle();

    // Integration tests
    test_group_by_query();
    test_count_star();
    test_schema_encodings();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run
              << " tests passed ===\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
