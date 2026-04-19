# Columnar-OLAP-Storage-Engine
A high-performance columnar query engine written in C++ that demonstrates the fundamental tradeoff between row-oriented and column-oriented storage for analytical workloads. Achieves 15x faster aggregations than row-oriented baselines while using 2.75x less disk space through dictionary encoding and run-length encoding compression.
# Why This Exists
Traditional row-oriented databases (PostgreSQL, MySQL) excel at transactional workloads but struggle with analytical queries that scan millions of rows but only need a few columns.
