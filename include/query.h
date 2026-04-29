#pragma once
#include <string>
#include <vector>
#include <optional>

// ── Query representation ──────────────────────────────────────────────────────

enum class AggFunc { NONE, COUNT, SUM, AVG, MIN, MAX };
enum class CmpOp   { EQ, NE, LT, LE, GT, GE };

struct SelectExpr {
    bool    star       = false;   // SELECT *
    bool    count_star = false;   // COUNT(*)
    AggFunc agg        = AggFunc::NONE;
    std::string column;           // empty for COUNT(*)
};

struct Predicate {
    std::string col;
    CmpOp       op;
    std::string literal;  // raw string; executor converts to the right type
};

struct Query {
    std::vector<SelectExpr>  select;
    std::string              table;
    std::optional<Predicate> where;
    std::string              group_by;  // empty = no GROUP BY
};

// ── Parser ───────────────────────────────────────────────────────────────────

// Parse a QUERY string (without the leading "QUERY" keyword).
// The caller strips "QUERY " before passing the rest.
// Returns false and fills err on syntax error.
bool parse_query(const std::string& text, Query& out, std::string& err);

// ── Executor ─────────────────────────────────────────────────────────────────

// Execute a parsed query against the warehouse directory.
// Prints results to stdout. Returns false on error.
bool execute_query(const Query& q, const std::string& warehouse_dir);