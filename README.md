# Columnar OLAP Storage Engine

> **Advanced Database Management — Project 02**
> Built in C++17

A from-scratch analytical database engine that stores tabular data in a columnar format and answers aggregation queries against it. Each column lives in its own binary file, so queries that touch only a few columns out of many read only those files — the core idea behind data warehouses like Parquet, DuckDB, ClickHouse, and BigQuery.

---

## Table of Contents

- [Overview](#overview)
- [Building](#building)
- [Running the Shell](#running-the-shell)
- [Shell Commands](#shell-commands)
- [Query DSL](#query-dsl)
- [Column File Format](#column-file-format)
- [Encoding Strategy](#encoding-strategy)
- [Project Structure](#project-structure)
- [Running the Benchmark](#running-the-benchmark)
- [Running Tests](#running-tests)
- [Limitations](#limitations)

---

## Overview

The engine has three phases, each building on the last:

| Phase | What it delivers |
|-------|-----------------|
| **Phase 1** | CSV loader, raw (no-encoding) column files, query executor, `colsh` shell |
| **Phase 2** | Dictionary encoding for strings, RLE for sorted numerics, GROUP BY support |
| **Phase 3** | Row-oriented baseline engine, `BENCH run` command, benchmark dataset generator |

The signature result of the benchmark is that analytical queries (SUM, AVG, GROUP BY) run roughly **10–15× faster** on the column store than on the row store because they physically read less data, while a single-row point lookup (`SELECT * WHERE id = N`) is **slower** on the column store because it must open every column file to reconstruct one row.

---

## Building

### Requirements

- GCC 11+ or Clang 13+ with C++17 support
- `make`
- Tested on Ubuntu 22.04 LTS and 24.04 LTS

### Build

```bash
git clone <repo-url>
cd <repo-dir>
make
```

This produces three binaries:

| Binary | Purpose |
|--------|---------|
| `colsh` | Interactive shell for the column store |
| `rowbench` | Row-oriented baseline engine (used by the benchmark) |
| `genbench` | Benchmark dataset generator (writes CSV to stdout) |

To build with debug symbols:

```bash
make debug
```

To clean:

```bash
make clean
```

---

## Running the Shell

```bash
./colsh --data ./warehouse
```

`--data` sets the directory where per-table subdirectories will be written. The directory is created if it does not exist.

```
colsh > LOAD sales.csv AS sales
colsh > QUERY SELECT country, SUM(price) FROM sales WHERE date >= 20240101 GROUP BY country
colsh > BENCH run
colsh > \quit
```

---

## Shell Commands

### `LOAD <file.csv> AS <table>`

Reads a CSV file, infers column types, selects an encoding per column, and writes binary column files into `<data-dir>/<table>/`. Prints a per-column size breakdown on completion.

```
colsh > LOAD sales.csv AS sales
Loaded 1,000,000 rows, 7 columns into ./warehouse/sales/
column      type    encoding   size
----------- ------- ---------- -----------
id          int64   none       8.0 MB
date        int32   rle        0.3 MB (365 runs)
country     string  dict       0.2 MB (24 distinct)
...
```

### `QUERY <sql>`

Runs a query against a loaded table. See [Query DSL](#query-dsl) below. Prints results, elapsed time, and how many columns and bytes were read from disk.

```
colsh > QUERY SELECT COUNT(*) FROM sales WHERE category = 'Electronics'
count
-------
82134
(1 row, 12 ms)
[read 1 of 7 columns, 0.3 MB from disk]
```

### `BENCH run`

Loads the benchmark dataset into both the column store and the row-oriented baseline (generating it if not already present), runs the three required queries on each, and prints a side-by-side timing comparison.

### `\stats`

Prints the list of loaded tables, their column counts, row counts, and total on-disk size.

### `\quit` / `\exit`

Exits the shell.

---

## Query DSL

The engine accepts a small, fixed subset of SQL. There is no full SQL parser — the grammar is hand-written and intentionally limited.

```
SELECT  select_list
FROM    table_name
[WHERE  column op literal]
[GROUP BY column]
```

### select_list

| Form | Example |
|------|---------|
| All columns | `SELECT *` |
| Specific columns | `SELECT country, price` |
| Aggregate | `SELECT SUM(price)`, `SELECT COUNT(*)`, `SELECT AVG(quantity)` |
| Mixed | `SELECT country, SUM(price)` |

Supported aggregates: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`.

### WHERE clause

Single predicate only. Operators: `=`, `!=`, `<`, `<=`, `>`, `>=`.

```sql
WHERE date >= 20240101
WHERE category = 'Electronics'
WHERE id = 500000
```

String literals must be single-quoted.

### GROUP BY

One column. Must be used with at least one aggregate in the select list.

```sql
SELECT country, AVG(quantity) FROM sales GROUP BY country
```

### Example queries

```sql
-- Analytical aggregate (reads 2 of 7 columns)
QUERY SELECT SUM(price) FROM sales WHERE date >= 20240101

-- Grouped aggregate (reads 2 of 7 columns)
QUERY SELECT country, AVG(quantity) FROM sales GROUP BY country

-- Filtered count (reads 1 of 7 columns)
QUERY SELECT COUNT(*) FROM sales WHERE category = 'Electronics'

-- Point lookup (reads all 7 columns — deliberately slow)
QUERY SELECT * FROM sales WHERE id = 500000
```

---

## Column File Format

Each column is stored as a single binary file: `<data-dir>/<table>/<column>.col`.

```
Offset  Size  Field        Description
------  ----  -----------  ---------------------------------
0       4     magic        ASCII 'C','O','L','1'
4       4     version      uint32, must be 1
8       1     data_type    uint8 (1=INT32, 2=INT64, 3=DOUBLE, 4=STRING)
9       1     encoding     uint8 (0=none, 1=dict, 2=rle)
10      2     reserved     uint16, must be 0
12      8     row_count    uint64, total rows
20      8     data_size    uint64, bytes in data section
28      8     dict_size    uint64, bytes in dict section (0 if n/a)
36      ...   data section encoded values
...     ...   dict section only present if encoding = 1
...     8     footer_crc64 CRC64 over all preceding bytes
```

A `schema.json` file in each table directory lists all columns in order with their names, types, and encodings. The query executor reads this file first. If `schema.json` is absent, the table is treated as not present.

---

## Encoding Strategy

The loader automatically picks an encoding per column using deterministic rules:

| Column type | Condition | Encoding chosen |
|-------------|-----------|-----------------|
| STRING | distinct values ≤ 65,535 | **Dictionary** — stores integer IDs in data section, string map at end |
| STRING | distinct values > 65,535 | None (raw strings) |
| Numeric | run count < N/4 | **RLE** — stores (value, run\_length) pairs |
| Numeric | otherwise | **None** — raw binary values |

Dictionary encoding typically shrinks string columns by 10–50×. RLE nearly eliminates sorted or low-cardinality numeric columns (e.g., a date column sorted by day compresses from 4 MB to ~0.3 MB).

---

## Project Structure

```
.
├── Makefile
├── README.md
├── src/
│   ├── main.cpp           # colsh shell entry point
│   ├── loader.cpp/h       # CSV parser, type inference, column writer
│   ├── encoding.cpp/h     # Dictionary builder, RLE encoder/decoder
│   ├── column_file.cpp/h  # File format: read & write, CRC64
│   ├── schema.cpp/h       # schema.json writer & reader
│   ├── query_parser.cpp/h # Recursive descent DSL parser
│   ├── executor.cpp/h     # Column reader, predicate, aggregation, group by
│   ├── rowstore.cpp/h     # Row-oriented baseline engine
│   └── bench.cpp/h        # BENCH run implementation
├── benchmark/
│   ├── genbench.cpp       # Benchmark CSV generator (fixed seed)
│   └── results.txt        # Output of the final benchmark run
└── tests/
    ├── test_csv.cpp
    ├── test_encoding.cpp
    ├── test_column_file.cpp
    ├── test_predicate.cpp
    ├── test_groupby.cpp
    └── test_e2e.cpp
```

---

## Running the Benchmark

Generate the dataset and run the benchmark in one step:

```bash
./genbench > benchmark/sales_bench.csv
./colsh --data ./warehouse
colsh > BENCH run
```

Or run `BENCH run` directly from the shell — it generates the dataset automatically if not found. The benchmark runs three queries on both engines side by side:

| Query | Expected result |
|-------|----------------|
| `SELECT SUM(price) WHERE date >= 20240101` | Column store ≥ 5× faster |
| `SELECT country, AVG(quantity) GROUP BY country` | Column store ≥ 5× faster |
| `SELECT * WHERE id = 500000` | **Row store is faster** (discusses the column-store tradeoff) |

You can also run the row-oriented baseline manually for point-lookup comparison:

```bash
./rowbench lookup sales id=500000
```

---

## Running Tests

```bash
make test
```

Tests cover:

- CSV parser (quoting, commas, type inference)
- Dictionary encoding round-trip (write → read)
- RLE encoding round-trip
- Column file CRC64 integrity check
- Predicate evaluator (all six operators, each type)
- GROUP BY aggregator correctness
- End-to-end: load a small table, run queries, verify results



