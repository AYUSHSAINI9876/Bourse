#include <string>
#include <vector>

#include "bourse/exec/command.hpp"
#include "bourse/sql/engine.hpp"

namespace bourse::exec {
namespace {

Reply noEngine() { return Reply::error("ERR the SQL engine is not enabled on this server"); }

/// Renders a ResultSet as a RESP array: a header row of column names followed
/// by one array per data row. Keeping the shape uniform means redis-cli prints
/// something readable without the server having to format a table.
Reply encodeResultSet(const sql::ResultSet& result) {
  if (result.columns.empty()) {
    return Reply::simpleString(result.message.empty() ? "OK" : result.message);
  }

  std::vector<Reply> out;
  out.reserve(result.rows.size() + 1);
  out.push_back(Reply::stringArray(result.columns));

  for (const sql::Row& row : result.rows) {
    std::vector<Reply> cells;
    cells.reserve(row.size());
    for (const sql::Datum& value : row) {
      if (value.isNull()) {
        cells.push_back(Reply::null());
      } else {
        cells.push_back(Reply::bulkString(value.toString()));
      }
    }
    out.push_back(Reply::array(std::move(cells)));
  }
  return Reply::array(std::move(out));
}

/// `SQL` takes the statement as free text, so the argv has to be rejoined.
/// Quoted arguments already had their quotes stripped by the RESP layer, so a
/// value containing spaces is re-quoted here rather than silently splitting.
std::string rejoin(const std::vector<std::string>& argv) {
  std::string statement;
  for (std::size_t i = 1; i < argv.size(); ++i) {
    if (i > 1) {
      statement.push_back(' ');
    }
    statement.append(argv[i]);
  }
  return statement;
}

void add(CommandRegistry& registry, std::string name, int arity, bool is_write, std::string summary,
         LambdaCommand::Handler handler) {
  registry.registerCommand(std::make_unique<LambdaCommand>(std::move(name), arity, is_write,
                                                           std::move(summary), std::move(handler)));
}

}  // namespace

void registerSqlCommands(CommandRegistry& registry) {
  // Marked as a write verb so the WAL journals it. Replay then re-executes the
  // statement, which reconstructs the tables exactly.
  add(registry, "SQL", -2, true, "SQL <statement>",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if (ctx.server.sql_engine == nullptr) {
          return noEngine();
        }
        Result<sql::ResultSet> result = ctx.server.sql_engine->execute(rejoin(argv));
        if (!result.ok()) {
          return Reply::error("ERR " + result.status().message());
        }
        return encodeResultSet(result.value());
      });

  add(registry, "EXPLAIN", -2, false, "EXPLAIN <select statement>",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if (ctx.server.sql_engine == nullptr) {
          return noEngine();
        }
        Result<sql::ResultSet> result = ctx.server.sql_engine->execute(rejoin(argv));
        if (!result.ok()) {
          return Reply::error("ERR " + result.status().message());
        }
        // Every plan node describes itself, so EXPLAIN costs nothing extra.
        return Reply::bulkString(result.value().plan.empty() ? "no plan (not a query)"
                                                             : result.value().plan);
      });

  add(registry, "TABLES", 1, false, "TABLES",
      [](CommandContext& ctx, const std::vector<std::string>&) {
        if (ctx.server.sql_engine == nullptr) {
          return noEngine();
        }
        return Reply::stringArray(ctx.server.sql_engine->tableNames());
      });

  add(registry, "DESCRIBE", 2, false, "DESCRIBE <table>",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if (ctx.server.sql_engine == nullptr) {
          return noEngine();
        }
        std::optional<sql::TableSchema> schema = ctx.server.sql_engine->schemaOf(argv[1]);
        if (!schema.has_value()) {
          return Reply::error("ERR no such table: " + argv[1]);
        }
        std::vector<Reply> out;
        for (const sql::ColumnDef& column : schema->columns) {
          std::string description = std::string(sql::toString(column.type));
          if (column.primary_key) {
            description.append(" PRIMARY KEY");
          } else if (column.not_null) {
            description.append(" NOT NULL");
          }
          out.push_back(Reply::bulkString(column.name));
          out.push_back(Reply::bulkString(description));
        }
        return Reply::array(std::move(out));
      });
}

}  // namespace bourse::exec
