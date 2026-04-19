# Columnar-OLAP-Storage-Engine
A high-performance columnar query engine written in C++ that demonstrates the fundamental tradeoff between row-oriented and column-oriented storage for analytical workloads. Achieves 15x faster aggregations than row-oriented baselines while using 2.75x less disk space through dictionary encoding and run-length encoding compression.
# Why This Exists
Traditional row-oriented databases (PostgreSQL, MySQL) excel at transactional workloads but struggle with analytical queries that scan millions of rows but only need a few columns.
# Architecture
┌─────────────────────────────────────────────────────────────┐
│                         colsh Shell                          │
│  LOAD, QUERY, BENCH, STATS                                   │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Query Executor                          │
│  • Predicate pushdown (WHERE column OP literal)             │
│  • Bitmap filtering                                          │
│  • Hash aggregation (GROUP BY)                               │
│  • Only reads columns referenced in query                    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                     Column Readers                           │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐         │
│  │    Raw       │ │  Dictionary  │ │     RLE      │         │
│  │   Reader     │ │   Reader     │ │   Reader     │         │
│  └──────────────┘ └──────────────┘ └──────────────┘         │
│         ↓                ↓                ↓                  │
│    No compression    Strings → IDs    (value, run) pairs    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   Column Files (per column)                  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐    │
│  │ id.col │ │date.col│ │country │ │price.c │ │ qty.co │    │
│  │        │ │        │ │  .col  │ │  ol    │ │  l     │    │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘    │
│                                                              │
│  schema.json → column names, types, encodings               │
└─────────────────────────────────────────────────────────────┘
# Compression Strategies

The engine automatically selects the optimal encoding per column:

# Dictionary Encoding (Strings)

Input:   "USA", "Canada", "USA", "Mexico", "Canada", "USA"
Store:   [0, 1, 0, 2, 1, 0] + dict["USA","Canada","Mexico"]
Result:  ~80% reduction for low-cardinality columns

# Run-Length Encoding (Sorted Numerics)

Input:   2024-01-01 (x1000), 2024-01-02 (x1000), 2024-01-03 (x1000)
Store:   (2024-01-01, 1000), (2024-01-02, 1000), (2024-01-03, 1000)
Result:  ~99% reduction for sorted date columns

# Raw Storage (High-Cardinality Numerics)

Input:   price = 19.99, 23.45, 17.89, 34.50...
Store:   Raw binary values (no overhead, no benefit)
# File Format

Each column is stored in a binary file:
┌──────────────────────────────────────────────────────────────┐
│ Header (32 bytes)                                            │
│  • Magic bytes "COLS"                                        │
│  • Version (1)                                               │
│  • Column type (int32/double/string)                         │
│  • Encoding (0=none, 1=dict, 2=RLE)                         │
│  • Value count                                               │
│  • Data offset                                               │
├──────────────────────────────────────────────────────────────┤
│ Dictionary (if encoding == 1)                                │
│  • Entry count                                               │
│  • For each: length + bytes                                 │
├──────────────────────────────────────────────────────────────┤
│ Data Section                                                 │
│  • Raw: values packed consecutively                          │
│  • Dictionary: small integers (1/2/4 bytes based on dict size)│
│  • RLE: (value, run_length) pairs                           │
├──────────────────────────────────────────────────────────────┤
│ Footer                                                       │
│  • CRC64 checksum                                            │
└──────────────────────────────────────────────────────────────┘

# Query Language (DSL)

A minimal SQL subset with a hand-written recursive descent parser:
-- Analytical queries (column store wins)
SELECT SUM(price) FROM sales WHERE date >= 20240101;
SELECT country, AVG(quantity) FROM sales GROUP BY country;
SELECT COUNT(*) FROM sales WHERE category = 'Electronics';

-- Point lookup (row store wins - demonstrated in benchmark)
SELECT * FROM sales WHERE id = 500000;

