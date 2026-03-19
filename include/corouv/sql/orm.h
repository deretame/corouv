#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "corouv/sql.h"

namespace corouv::sql::orm {

struct JoinSpec {
    std::string kind{"INNER"};
    std::string table;
    std::string alias;
    std::string on;
};

struct TableMetaRuntime {
    std::string table;
    std::vector<std::string> columns;
    std::optional<std::string> primary_key;
};

struct WhereClause {
    std::string sql;
    Params params;
    // true: convert '?' to backend placeholders (e.g. $1 for pg)
    // false: keep sql as-is (useful for backend-specific raw SQL)
    bool rewrite_question_placeholders{true};

    [[nodiscard]] bool empty() const noexcept { return sql.empty(); }
};

enum class SortDirection {
    Asc,
    Desc,
};

struct OrderBy {
    std::string expr;
    SortDirection direction{SortDirection::Asc};
};

struct SelectOptions {
    std::string select_expr;
    std::optional<WhereClause> where;
    std::vector<std::string> group_by;
    std::optional<WhereClause> having;
    std::vector<OrderBy> order_by;
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
};

struct OneToOneRelation {
    std::string target_table;
    std::string target_alias;
    std::string source_key;
    std::string target_key;
    std::string kind{"LEFT"};
};

struct OneToManyRelation {
    std::string target_table;
    std::string target_alias;
    std::string source_key;
    std::string target_key;
    std::string kind{"LEFT"};
};

struct ManyToManyRelation {
    std::string junction_table;
    std::string junction_alias;
    std::string target_table;
    std::string target_alias;

    std::string source_key;
    std::string junction_source_key;
    std::string junction_target_key;
    std::string target_key;

    std::string junction_kind{"LEFT"};
    std::string target_kind{"LEFT"};
};

inline std::string placeholder(Backend backend, std::size_t index) {
    if (backend == Backend::PostgreSQL) {
        return "$" + std::to_string(index);
    }
    return "?";
}

inline std::string placeholders(Backend backend, std::size_t count,
                                std::size_t start_index = 1) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out.append(", ");
        }
        out.append(placeholder(backend, start_index + i));
    }
    return out;
}

inline std::string question_placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out.append(", ");
        }
        out.push_back('?');
    }
    return out;
}

inline std::string join_columns(const std::vector<std::string>& columns) {
    std::string out;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            out.append(", ");
        }
        out.append(columns[i]);
    }
    return out;
}

inline std::string joins_sql(const std::vector<JoinSpec>& joins) {
    std::string out;
    for (const auto& join : joins) {
        if (join.table.empty() || join.on.empty()) {
            continue;
        }
        out.push_back(' ');
        out.append(join.kind.empty() ? "INNER" : join.kind);
        out.append(" JOIN ");
        out.append(join.table);
        if (!join.alias.empty()) {
            out.push_back(' ');
            out.append(join.alias);
        }
        out.append(" ON ");
        out.append(join.on);
    }
    return out;
}

inline std::string append_where(std::string sql, std::string_view where) {
    if (!where.empty()) {
        sql.append(" WHERE ");
        sql.append(where);
    }
    return sql;
}

inline void check_columns(const TableMetaRuntime& table) {
    if (table.table.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty table name");
    }
    if (table.columns.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty column list");
    }
}

inline std::size_t count_question_placeholders(std::string_view sql) {
    std::size_t count = 0;
    for (char ch : sql) {
        if (ch == '?') {
            ++count;
        }
    }
    return count;
}

inline void validate_where_clause(const WhereClause& where) {
    if (where.empty()) {
        if (!where.params.empty()) {
            throw std::invalid_argument(
                "corouv::sql::orm empty where clause with non-empty params");
        }
        return;
    }

    if (!where.rewrite_question_placeholders) {
        return;
    }

    const std::size_t expected = count_question_placeholders(where.sql);
    if (expected != where.params.size()) {
        throw std::invalid_argument(
            "corouv::sql::orm where clause placeholder count mismatch");
    }
}

inline std::string render_question_placeholders(std::string_view sql,
                                                Backend backend,
                                                std::size_t start_index) {
    if (backend != Backend::PostgreSQL) {
        return std::string(sql);
    }

    std::string out;
    out.reserve(sql.size() + 16);

    std::size_t index = start_index;
    for (char ch : sql) {
        if (ch == '?') {
            out.append("$");
            out.append(std::to_string(index));
            ++index;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

inline std::string render_where_clause(const WhereClause& where, Backend backend,
                                       std::size_t start_index = 1) {
    validate_where_clause(where);
    if (!where.rewrite_question_placeholders) {
        return where.sql;
    }
    return render_question_placeholders(where.sql, backend, start_index);
}

inline void append_params(Params& out, const Params& in) {
    out.insert(out.end(), in.begin(), in.end());
}

inline std::string qualify_column(std::string_view ref, std::string_view column) {
    if (ref.empty()) {
        return std::string(column);
    }
    std::string out;
    out.reserve(ref.size() + 1 + column.size());
    out.append(ref);
    out.push_back('.');
    out.append(column);
    return out;
}

inline WhereClause where_raw(std::string sql, Params params = {},
                             bool rewrite_question_placeholders = false) {
    WhereClause out;
    out.sql = std::move(sql);
    out.params = std::move(params);
    out.rewrite_question_placeholders = rewrite_question_placeholders;
    return out;
}

inline WhereClause where_binary(std::string column, std::string op, Value value) {
    if (column.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty where column");
    }
    WhereClause out;
    out.sql = std::move(column);
    out.sql.push_back(' ');
    out.sql.append(op);
    out.sql.append(" ?");
    out.params.push_back(std::move(value));
    return out;
}

inline WhereClause where_eq(std::string column, Value value) {
    return where_binary(std::move(column), "=", std::move(value));
}

inline WhereClause where_ne(std::string column, Value value) {
    return where_binary(std::move(column), "!=", std::move(value));
}

inline WhereClause where_gt(std::string column, Value value) {
    return where_binary(std::move(column), ">", std::move(value));
}

inline WhereClause where_ge(std::string column, Value value) {
    return where_binary(std::move(column), ">=", std::move(value));
}

inline WhereClause where_lt(std::string column, Value value) {
    return where_binary(std::move(column), "<", std::move(value));
}

inline WhereClause where_le(std::string column, Value value) {
    return where_binary(std::move(column), "<=", std::move(value));
}

inline WhereClause where_like(std::string column, Value value) {
    return where_binary(std::move(column), "LIKE", std::move(value));
}

inline WhereClause where_between(std::string column, Value low, Value high) {
    if (column.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty where column");
    }
    WhereClause out;
    out.sql = std::move(column);
    out.sql.append(" BETWEEN ? AND ?");
    out.params.push_back(std::move(low));
    out.params.push_back(std::move(high));
    return out;
}

inline WhereClause where_in(std::string column, Params values) {
    if (column.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty where column");
    }
    if (values.empty()) {
        throw std::invalid_argument("corouv::sql::orm where_in values is empty");
    }

    WhereClause out;
    out.sql = std::move(column);
    out.sql.append(" IN (");
    out.sql.append(question_placeholders(values.size()));
    out.sql.push_back(')');
    out.params = std::move(values);
    return out;
}

inline WhereClause where_is_null(std::string column) {
    if (column.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty where column");
    }
    WhereClause out;
    out.sql = std::move(column);
    out.sql.append(" IS NULL");
    return out;
}

inline WhereClause where_is_not_null(std::string column) {
    if (column.empty()) {
        throw std::invalid_argument("corouv::sql::orm empty where column");
    }
    WhereClause out;
    out.sql = std::move(column);
    out.sql.append(" IS NOT NULL");
    return out;
}

inline WhereClause where_not(WhereClause clause) {
    validate_where_clause(clause);
    if (clause.empty()) {
        return clause;
    }

    WhereClause out;
    out.sql = "NOT (" + clause.sql + ")";
    out.params = std::move(clause.params);
    out.rewrite_question_placeholders = clause.rewrite_question_placeholders;
    return out;
}

inline WhereClause where_combine(std::vector<WhereClause> clauses,
                                 std::string_view op) {
    WhereClause out;

    bool first = true;
    std::optional<bool> rewrite_mode;

    for (auto& clause : clauses) {
        if (clause.empty()) {
            continue;
        }
        validate_where_clause(clause);

        if (!rewrite_mode.has_value()) {
            rewrite_mode = clause.rewrite_question_placeholders;
        } else if (*rewrite_mode != clause.rewrite_question_placeholders) {
            // Clauses without params do not affect placeholder rendering mode.
            if (!clause.params.empty()) {
                if (out.params.empty()) {
                    rewrite_mode = clause.rewrite_question_placeholders;
                } else {
                    throw std::invalid_argument(
                        "corouv::sql::orm mixed placeholder rewrite mode in "
                        "where_combine");
                }
            }
        }

        if (!first) {
            out.sql.push_back(' ');
            out.sql.append(op);
            out.sql.push_back(' ');
        }
        out.sql.push_back('(');
        out.sql.append(clause.sql);
        out.sql.push_back(')');

        append_params(out.params, clause.params);
        first = false;
    }

    if (rewrite_mode.has_value()) {
        out.rewrite_question_placeholders = *rewrite_mode;
    }

    return out;
}

inline WhereClause where_and(std::vector<WhereClause> clauses) {
    return where_combine(std::move(clauses), "AND");
}

inline WhereClause where_or(std::vector<WhereClause> clauses) {
    return where_combine(std::move(clauses), "OR");
}

inline std::string order_by_sql(const std::vector<OrderBy>& terms) {
    std::string out;
    bool first = true;

    for (const auto& term : terms) {
        if (term.expr.empty()) {
            continue;
        }
        if (first) {
            out.append(" ORDER BY ");
            first = false;
        } else {
            out.append(", ");
        }

        out.append(term.expr);
        if (term.direction == SortDirection::Desc) {
            out.append(" DESC");
        } else {
            out.append(" ASC");
        }
    }

    return out;
}

inline std::string group_by_sql(const std::vector<std::string>& groups) {
    std::string out;
    bool first = true;

    for (const auto& group : groups) {
        if (group.empty()) {
            continue;
        }
        if (first) {
            out.append(" GROUP BY ");
            first = false;
        } else {
            out.append(", ");
        }
        out.append(group);
    }

    return out;
}

inline JoinSpec build_join_from_relation(std::string_view source_ref,
                                         std::string target_table,
                                         std::string target_alias,
                                         std::string source_key,
                                         std::string target_key,
                                         std::string kind) {
    if (source_ref.empty() || target_table.empty() || source_key.empty() ||
        target_key.empty()) {
        throw std::invalid_argument(
            "corouv::sql::orm invalid relation specification");
    }

    JoinSpec join;
    join.kind = kind.empty() ? "LEFT" : std::move(kind);
    join.table = std::move(target_table);
    join.alias = std::move(target_alias);

    const std::string rhs_ref = join.alias.empty() ? join.table : join.alias;
    join.on = qualify_column(rhs_ref, target_key);
    join.on.append(" = ");
    join.on.append(qualify_column(source_ref, source_key));

    return join;
}

inline std::vector<JoinSpec> relation_joins(std::string_view source_ref,
                                            const OneToOneRelation& relation) {
    std::vector<JoinSpec> joins;
    joins.push_back(build_join_from_relation(
        source_ref, relation.target_table, relation.target_alias,
        relation.source_key, relation.target_key, relation.kind));
    return joins;
}

inline std::vector<JoinSpec> relation_joins(std::string_view source_ref,
                                            const OneToManyRelation& relation) {
    std::vector<JoinSpec> joins;
    joins.push_back(build_join_from_relation(
        source_ref, relation.target_table, relation.target_alias,
        relation.source_key, relation.target_key, relation.kind));
    return joins;
}

inline std::vector<JoinSpec> relation_joins(std::string_view source_ref,
                                            const ManyToManyRelation& relation) {
    if (source_ref.empty() || relation.junction_table.empty() ||
        relation.target_table.empty() || relation.source_key.empty() ||
        relation.junction_source_key.empty() || relation.junction_target_key.empty() ||
        relation.target_key.empty()) {
        throw std::invalid_argument(
            "corouv::sql::orm invalid many-to-many relation specification");
    }

    const std::string junction_ref =
        relation.junction_alias.empty() ? relation.junction_table
                                        : relation.junction_alias;
    const std::string target_ref =
        relation.target_alias.empty() ? relation.target_table
                                      : relation.target_alias;

    std::vector<JoinSpec> joins;

    JoinSpec join_junction;
    join_junction.kind = relation.junction_kind.empty() ? "LEFT"
                                                        : relation.junction_kind;
    join_junction.table = relation.junction_table;
    join_junction.alias = relation.junction_alias;
    join_junction.on = qualify_column(junction_ref, relation.junction_source_key);
    join_junction.on.append(" = ");
    join_junction.on.append(qualify_column(source_ref, relation.source_key));
    joins.push_back(std::move(join_junction));

    JoinSpec join_target;
    join_target.kind = relation.target_kind.empty() ? "LEFT" : relation.target_kind;
    join_target.table = relation.target_table;
    join_target.alias = relation.target_alias;
    join_target.on = qualify_column(target_ref, relation.target_key);
    join_target.on.append(" = ");
    join_target.on.append(
        qualify_column(junction_ref, relation.junction_target_key));
    joins.push_back(std::move(join_target));

    return joins;
}

inline void append_optional_clause(std::string& sql, Backend backend,
                                   std::size_t& placeholder_index,
                                   Params& params,
                                   const std::optional<WhereClause>& clause,
                                   std::string_view keyword) {
    if (!clause.has_value() || clause->empty()) {
        return;
    }

    sql.push_back(' ');
    sql.append(keyword);
    sql.push_back(' ');
    sql.append(render_where_clause(*clause, backend, placeholder_index));

    append_params(params, clause->params);
    placeholder_index += clause->params.size();
}

inline Task<Result> select_with_joins(Connection& conn,
                                      const TableMetaRuntime& base,
                                      const std::vector<JoinSpec>& joins,
                                      SelectOptions options = {}) {
    check_columns(base);

    std::string sql = "SELECT ";
    if (options.select_expr.empty()) {
        sql.append(join_columns(base.columns));
    } else {
        sql.append(options.select_expr);
    }

    sql.append(" FROM ");
    sql.append(base.table);
    sql.append(joins_sql(joins));

    Params params;
    std::size_t placeholder_index = 1;

    append_optional_clause(sql, conn.backend(), placeholder_index, params,
                           options.where, "WHERE");

    sql.append(group_by_sql(options.group_by));

    append_optional_clause(sql, conn.backend(), placeholder_index, params,
                           options.having, "HAVING");

    sql.append(order_by_sql(options.order_by));

    if (options.limit.has_value()) {
        sql.append(" LIMIT ");
        sql.append(std::to_string(*options.limit));
    }
    if (options.offset.has_value()) {
        sql.append(" OFFSET ");
        sql.append(std::to_string(*options.offset));
    }

    co_return co_await conn.query(std::move(sql), std::move(params));
}

inline Task<Result> select(Connection& conn, const TableMetaRuntime& table,
                           SelectOptions options = {}) {
    std::vector<JoinSpec> joins;
    co_return co_await select_with_joins(conn, table, joins, std::move(options));
}

inline Task<Result> find_all(Connection& conn, const TableMetaRuntime& table,
                             std::string where = {}, Params params = {}) {
    check_columns(table);
    std::string sql = "SELECT " + join_columns(table.columns) + " FROM " + table.table;
    sql = append_where(std::move(sql), where);
    co_return co_await conn.query(std::move(sql), std::move(params));
}

inline Task<Result> find_by_pk(Connection& conn, const TableMetaRuntime& table,
                               Value pk_value) {
    check_columns(table);
    if (!table.primary_key.has_value() || table.primary_key->empty()) {
        throw std::invalid_argument("corouv::sql::orm primary key is required");
    }
    std::string sql = "SELECT " + join_columns(table.columns) + " FROM " + table.table +
                      " WHERE " + *table.primary_key + " = " +
                      placeholder(conn.backend(), 1) + " LIMIT 1";
    Params params;
    params.push_back(std::move(pk_value));
    co_return co_await conn.query(std::move(sql), std::move(params));
}

inline Task<std::int64_t> insert(Connection& conn, const TableMetaRuntime& table,
                                 Params values) {
    check_columns(table);
    if (values.size() != table.columns.size()) {
        throw std::invalid_argument(
            "corouv::sql::orm insert values count does not match columns");
    }

    std::string sql = "INSERT INTO " + table.table + " (" + join_columns(table.columns) +
                      ") VALUES (" + placeholders(conn.backend(), table.columns.size()) +
                      ")";
    co_return co_await conn.execute(std::move(sql), std::move(values));
}

inline Task<std::int64_t> update_where(Connection& conn,
                                       const TableMetaRuntime& table,
                                       const std::vector<std::string>& set_columns,
                                       Params set_values, WhereClause where,
                                       bool allow_empty_where = false) {
    check_columns(table);
    if (set_columns.empty()) {
        throw std::invalid_argument("corouv::sql::orm update set_columns is empty");
    }
    if (set_values.size() != set_columns.size()) {
        throw std::invalid_argument(
            "corouv::sql::orm update values count does not match columns");
    }

    if (where.empty() && !allow_empty_where) {
        throw std::invalid_argument(
            "corouv::sql::orm update_where requires where clause");
    }

    std::string sql = "UPDATE " + table.table + " SET ";
    for (std::size_t i = 0; i < set_columns.size(); ++i) {
        if (i > 0) {
            sql.append(", ");
        }
        sql.append(set_columns[i]);
        sql.append(" = ");
        sql.append(placeholder(conn.backend(), i + 1));
    }

    if (!where.empty()) {
        sql.append(" WHERE ");
        sql.append(render_where_clause(where, conn.backend(), set_columns.size() + 1));
        append_params(set_values, where.params);
    }

    co_return co_await conn.execute(std::move(sql), std::move(set_values));
}

inline Task<std::int64_t> update_by_pk(Connection& conn,
                                       const TableMetaRuntime& table,
                                       const std::vector<std::string>& set_columns,
                                       Params set_values, Value pk_value) {
    check_columns(table);
    if (!table.primary_key.has_value() || table.primary_key->empty()) {
        throw std::invalid_argument("corouv::sql::orm primary key is required");
    }
    if (set_columns.empty()) {
        throw std::invalid_argument("corouv::sql::orm update set_columns is empty");
    }
    if (set_values.size() != set_columns.size()) {
        throw std::invalid_argument(
            "corouv::sql::orm update values count does not match columns");
    }

    std::string sql = "UPDATE " + table.table + " SET ";
    for (std::size_t i = 0; i < set_columns.size(); ++i) {
        if (i > 0) {
            sql.append(", ");
        }
        sql.append(set_columns[i]);
        sql.append(" = ");
        sql.append(placeholder(conn.backend(), i + 1));
    }
    sql.append(" WHERE ");
    sql.append(*table.primary_key);
    sql.append(" = ");
    sql.append(placeholder(conn.backend(), set_columns.size() + 1));

    set_values.push_back(std::move(pk_value));
    co_return co_await conn.execute(std::move(sql), std::move(set_values));
}

inline Task<std::int64_t> update_by_pk(Connection& conn,
                                       const TableMetaRuntime& table,
                                       Params set_values, Value pk_value) {
    co_return co_await update_by_pk(conn, table, table.columns,
                                    std::move(set_values), std::move(pk_value));
}

inline Task<std::int64_t> delete_where(Connection& conn,
                                       const TableMetaRuntime& table,
                                       WhereClause where,
                                       bool allow_empty_where = false) {
    check_columns(table);

    if (where.empty() && !allow_empty_where) {
        throw std::invalid_argument(
            "corouv::sql::orm delete_where requires where clause");
    }

    std::string sql = "DELETE FROM " + table.table;
    Params params;

    if (!where.empty()) {
        sql.append(" WHERE ");
        sql.append(render_where_clause(where, conn.backend(), 1));
        append_params(params, where.params);
    }

    co_return co_await conn.execute(std::move(sql), std::move(params));
}

inline Task<std::int64_t> delete_by_pk(Connection& conn,
                                       const TableMetaRuntime& table,
                                       Value pk_value) {
    check_columns(table);
    if (!table.primary_key.has_value() || table.primary_key->empty()) {
        throw std::invalid_argument("corouv::sql::orm primary key is required");
    }

    std::string sql = "DELETE FROM " + table.table + " WHERE " + *table.primary_key +
                      " = " + placeholder(conn.backend(), 1);
    Params params;
    params.push_back(std::move(pk_value));
    co_return co_await conn.execute(std::move(sql), std::move(params));
}

inline Task<Result> join_query(Connection& conn, const TableMetaRuntime& base,
                               const std::vector<JoinSpec>& joins,
                               std::string where = {}, Params params = {},
                               std::string select_expr = {}) {
    check_columns(base);

    std::string sql = "SELECT ";
    if (select_expr.empty()) {
        sql.append(join_columns(base.columns));
    } else {
        sql.append(select_expr);
    }
    sql.append(" FROM ");
    sql.append(base.table);
    sql.append(joins_sql(joins));
    sql = append_where(std::move(sql), where);

    co_return co_await conn.query(std::move(sql), std::move(params));
}

inline Task<Result> join_query(Connection& conn, const TableMetaRuntime& base,
                               const std::vector<JoinSpec>& joins,
                               SelectOptions options) {
    co_return co_await select_with_joins(conn, base, joins, std::move(options));
}

inline Task<Result> join_one_to_one(Connection& conn,
                                    const TableMetaRuntime& base,
                                    const OneToOneRelation& relation,
                                    SelectOptions options = {},
                                    std::string base_ref = {}) {
    check_columns(base);
    const std::string source_ref = base_ref.empty() ? base.table : base_ref;
    co_return co_await select_with_joins(
        conn, base, relation_joins(source_ref, relation), std::move(options));
}

inline Task<Result> join_one_to_many(Connection& conn,
                                     const TableMetaRuntime& base,
                                     const OneToManyRelation& relation,
                                     SelectOptions options = {},
                                     std::string base_ref = {}) {
    check_columns(base);
    const std::string source_ref = base_ref.empty() ? base.table : base_ref;
    co_return co_await select_with_joins(
        conn, base, relation_joins(source_ref, relation), std::move(options));
}

inline Task<Result> join_many_to_many(Connection& conn,
                                      const TableMetaRuntime& base,
                                      const ManyToManyRelation& relation,
                                      SelectOptions options = {},
                                      std::string base_ref = {}) {
    check_columns(base);
    const std::string source_ref = base_ref.empty() ? base.table : base_ref;
    co_return co_await select_with_joins(
        conn, base, relation_joins(source_ref, relation), std::move(options));
}

}  // namespace corouv::sql::orm
