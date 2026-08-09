#include "bourse/sql/engine.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "bourse/sql/parser.hpp"

namespace bourse::sql {
namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string lower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

/// The empty row used when evaluating expressions with no table in scope --
/// INSERT ... VALUES, and UPDATE right-hand sides that reference no column.
const Row& emptyRow() {
  static const Row kEmpty;
  return kEmpty;
}

const TableSchema& emptySchema() {
  static const TableSchema kEmpty;
  return kEmpty;
}

}  // namespace

// ---------------------------------------------------------------------------
// TableSchema / ResultSet
// ---------------------------------------------------------------------------

int TableSchema::indexOf(std::string_view column) const {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (equalsIgnoreCase(columns[i].name, column)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::vector<std::string> TableSchema::columnNames() const {
  std::vector<std::string> names;
  names.reserve(columns.size());
  for (const ColumnDef& column : columns) {
    names.push_back(column.name);
  }
  return names;
}

std::string ResultSet::toJson() const {
  std::string out = "{\"columns\":[";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.append(Datum::text(columns[i]).toJson());
  }
  out.append("],\"rows\":[");
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (r != 0) {
      out.push_back(',');
    }
    out.push_back('[');
    for (std::size_t c = 0; c < rows[r].size(); ++c) {
      if (c != 0) {
        out.push_back(',');
      }
      out.append(rows[r][c].toJson());
    }
    out.push_back(']');
  }
  out.append("],\"rows_affected\":").append(std::to_string(rows_affected));
  out.append(",\"message\":").append(Datum::text(message).toJson());
  // The plan travels with the result. Every node already describes itself, so
  // this costs nothing to produce, and without it `EXPLAIN` is reachable over
  // RESP but not over HTTP -- the dashboard could not show a query plan at all.
  out.append(",\"plan\":").append(Datum::text(plan).toJson());
  out.push_back('}');
  return out;
}

std::string ResultSet::toText() const {
  if (columns.empty()) {
    return message;
  }

  // Column widths first, so the table lines up regardless of content.
  std::vector<std::size_t> widths;
  widths.reserve(columns.size());
  for (const std::string& name : columns) {
    widths.push_back(name.size());
  }
  for (const Row& row : rows) {
    for (std::size_t c = 0; c < row.size() && c < widths.size(); ++c) {
      widths[c] = std::max(widths[c], row[c].toString().size());
    }
  }

  const auto pad = [](std::string text, std::size_t width) {
    text.resize(std::max(text.size(), width), ' ');
    return text;
  };

  std::string out;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    out.append(pad(columns[c], widths[c]));
    if (c + 1 < columns.size()) {
      out.append(" | ");
    }
  }
  out.push_back('\n');
  for (std::size_t c = 0; c < columns.size(); ++c) {
    out.append(std::string(widths[c], '-'));
    if (c + 1 < columns.size()) {
      out.append("-+-");
    }
  }
  out.push_back('\n');

  for (const Row& row : rows) {
    for (std::size_t c = 0; c < columns.size(); ++c) {
      out.append(pad(c < row.size() ? row[c].toString() : std::string{}, widths[c]));
      if (c + 1 < columns.size()) {
        out.append(" | ");
      }
    }
    out.push_back('\n');
  }
  out.append("(").append(std::to_string(rows.size())).append(rows.size() == 1 ? " row)" : " rows)");
  return out;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Table* Engine::findTable(const std::string& name) {
  auto it = tables_.find(lower(name));
  return it == tables_.end() ? nullptr : &it->second;
}

const Table* Engine::findTable(const std::string& name) const {
  auto it = tables_.find(lower(name));
  return it == tables_.end() ? nullptr : &it->second;
}

Result<ResultSet> Engine::execute(std::string_view sql) {
  Result<Statement> parsed = Parser::parse(sql);
  if (!parsed.ok()) {
    return parsed.status();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const Statement& statement = parsed.value();

  // std::visit over the statement variant. The expression tree uses Visitor
  // because its operations keep growing; statements use a variant because the
  // operation set is fixed at "execute it" and a closed set of alternatives is
  // simpler than a parallel class hierarchy.
  if (const auto* create = std::get_if<CreateTableStatement>(&statement)) {
    return runCreateTable(*create);
  }
  if (const auto* drop = std::get_if<DropTableStatement>(&statement)) {
    return runDropTable(*drop);
  }
  if (const auto* insert = std::get_if<InsertStatement>(&statement)) {
    return runInsert(*insert);
  }
  if (const auto* select = std::get_if<SelectStatement>(&statement)) {
    return runSelect(*select);
  }
  if (const auto* update = std::get_if<UpdateStatement>(&statement)) {
    return runUpdate(*update);
  }
  if (const auto* remove = std::get_if<DeleteStatement>(&statement)) {
    return runDelete(*remove);
  }
  return Status::internal("unhandled statement kind");
}

Result<ResultSet> Engine::runCreateTable(const CreateTableStatement& statement) {
  if (findTable(statement.table) != nullptr) {
    if (statement.if_not_exists) {
      ResultSet result;
      result.message = "table '" + statement.table + "' already exists";
      return result;
    }
    return Status::alreadyExists("table '" + statement.table + "' already exists");
  }

  // Duplicate column names would make every reference to them ambiguous.
  for (std::size_t i = 0; i < statement.columns.size(); ++i) {
    for (std::size_t j = i + 1; j < statement.columns.size(); ++j) {
      if (equalsIgnoreCase(statement.columns[i].name, statement.columns[j].name)) {
        return Status::invalidArgument("duplicate column name '" + statement.columns[i].name + "'");
      }
    }
  }

  Table table;
  table.schema.name = statement.table;
  table.schema.columns = statement.columns;
  tables_.emplace(lower(statement.table), std::move(table));

  ResultSet result;
  result.message = "created table '" + statement.table + "' with " +
                   std::to_string(statement.columns.size()) + " column(s)";
  return result;
}

Result<ResultSet> Engine::runDropTable(const DropTableStatement& statement) {
  auto it = tables_.find(lower(statement.table));
  if (it == tables_.end()) {
    if (statement.if_exists) {
      ResultSet result;
      result.message = "table '" + statement.table + "' does not exist";
      return result;
    }
    return Status::notFound("no such table: " + statement.table);
  }
  const std::size_t rows = it->second.rows.size();
  tables_.erase(it);

  ResultSet result;
  result.rows_affected = rows;
  result.message = "dropped table '" + statement.table + "'";
  return result;
}

Result<ResultSet> Engine::runInsert(const InsertStatement& statement) {
  Table* table = findTable(statement.table);
  if (table == nullptr) {
    return Status::notFound("no such table: " + statement.table);
  }

  // Resolve the target column order once, not per row.
  std::vector<int> targets;
  if (statement.columns.empty()) {
    targets.resize(table->schema.columns.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
      targets[i] = static_cast<int>(i);
    }
  } else {
    targets.reserve(statement.columns.size());
    for (const std::string& column : statement.columns) {
      const int index = table->schema.indexOf(column);
      if (index < 0) {
        return Status::invalidArgument("unknown column '" + column + "' in INSERT");
      }
      targets.push_back(index);
    }
  }

  // Build every row before touching the table, so a failure halfway through
  // leaves the table exactly as it was rather than half-inserted.
  std::vector<Row> staged;
  staged.reserve(statement.rows.size());

  for (const std::vector<ExpressionPtr>& source : statement.rows) {
    if (source.size() != targets.size()) {
      return Status::invalidArgument("expected " + std::to_string(targets.size()) + " value(s), got " +
                                     std::to_string(source.size()));
    }

    Row row(table->schema.columns.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
      Result<Datum> value = evaluate(*source[i], emptySchema(), emptyRow());
      if (!value.ok()) {
        return value.status();
      }
      const ColumnDef& column = table->schema.columns[static_cast<std::size_t>(targets[i])];
      Result<Datum> coerced = value.value().coerce(column.type);
      if (!coerced.ok()) {
        return Status::invalidArgument("column '" + column.name + "': " + coerced.status().message());
      }
      if (column.not_null && coerced.value().isNull()) {
        return Status::invalidArgument("column '" + column.name + "' is NOT NULL");
      }
      row[static_cast<std::size_t>(targets[i])] = std::move(coerced).value();
    }

    // NOT NULL applies to columns the statement did not mention, too.
    for (std::size_t i = 0; i < table->schema.columns.size(); ++i) {
      if (table->schema.columns[i].not_null && row[i].isNull()) {
        return Status::invalidArgument("column '" + table->schema.columns[i].name + "' is NOT NULL");
      }
    }

    // Primary keys must stay unique -- against the table and against the rows
    // staged so far in this same statement.
    for (std::size_t i = 0; i < table->schema.columns.size(); ++i) {
      if (!table->schema.columns[i].primary_key) {
        continue;
      }
      const auto clashes = [&](const Row& existing) {
        return i < existing.size() && Datum::compare(existing[i], row[i]).value_or(1) == 0;
      };
      if (std::any_of(table->rows.begin(), table->rows.end(), clashes) ||
          std::any_of(staged.begin(), staged.end(), clashes)) {
        return Status::alreadyExists("duplicate value for primary key '" + table->schema.columns[i].name +
                                     "'");
      }
    }

    staged.push_back(std::move(row));
  }

  const std::size_t inserted = staged.size();
  table->rows.insert(table->rows.end(), std::make_move_iterator(staged.begin()),
                     std::make_move_iterator(staged.end()));

  ResultSet result;
  result.rows_affected = inserted;
  result.message = "inserted " + std::to_string(inserted) + " row(s)";
  return result;
}

Result<ResultSet> Engine::runSelect(const SelectStatement& statement) {
  const Table* table = findTable(statement.table);
  if (table == nullptr) {
    return Status::notFound("no such table: " + statement.table);
  }

  // Build the plan bottom-up: scan -> filter -> sort -> limit.
  PlanNodePtr plan = makeSeqScan(*table);
  if (statement.where) {
    plan = makeFilter(std::move(plan), *statement.where, table->schema);
  }
  if (!statement.order_by.empty()) {
    plan = makeSort(std::move(plan), statement.order_by, table->schema);
  }
  if (statement.limit >= 0 || statement.offset > 0) {
    plan = makeLimit(std::move(plan), statement.limit, statement.offset);
  }

  // Projection happens outside the pipeline so that WHERE and ORDER BY can
  // still reference columns the SELECT list does not output.
  const bool wildcard = statement.items.size() == 1 && statement.items[0].expression == nullptr;

  ResultSet result;
  if (wildcard) {
    result.columns = table->schema.columnNames();
  } else {
    for (const SelectItem& item : statement.items) {
      if (item.expression == nullptr) {
        for (std::string& name : table->schema.columnNames()) {
          result.columns.push_back(std::move(name));
        }
        continue;
      }
      result.columns.push_back(item.alias.empty() ? expressionToString(*item.expression) : item.alias);
    }
  }

  plan->open();
  Row row;
  while (plan->next(row)) {
    if (wildcard) {
      result.rows.push_back(row);
      continue;
    }
    Row projected;
    projected.reserve(statement.items.size());
    for (const SelectItem& item : statement.items) {
      if (item.expression == nullptr) {
        for (const Datum& value : row) {
          projected.push_back(value);
        }
        continue;
      }
      Result<Datum> value = evaluate(*item.expression, table->schema, row);
      if (!value.ok()) {
        plan->close();
        return value.status();
      }
      projected.push_back(std::move(value).value());
    }
    result.rows.push_back(std::move(projected));
  }

  const Status plan_status = plan->status();
  result.plan = plan->describe();
  plan->close();
  if (!plan_status.ok()) {
    return plan_status;
  }

  result.message = std::to_string(result.rows.size()) + " row(s)";
  return result;
}

Result<ResultSet> Engine::runUpdate(const UpdateStatement& statement) {
  Table* table = findTable(statement.table);
  if (table == nullptr) {
    return Status::notFound("no such table: " + statement.table);
  }

  std::vector<int> targets;
  targets.reserve(statement.assignments.size());
  for (const auto& [column, value] : statement.assignments) {
    const int index = table->schema.indexOf(column);
    if (index < 0) {
      return Status::invalidArgument("unknown column '" + column + "' in UPDATE");
    }
    targets.push_back(index);
  }

  std::size_t affected = 0;
  for (Row& row : table->rows) {
    if (statement.where) {
      Result<Datum> verdict = evaluate(*statement.where, table->schema, row);
      if (!verdict.ok()) {
        return verdict.status();
      }
      if (!verdict.value().isTrue()) {
        continue;
      }
    }

    // Evaluate every assignment against the *pre-update* row so that
    // `SET a = b, b = a` swaps rather than duplicating.
    const Row before = row;
    for (std::size_t i = 0; i < statement.assignments.size(); ++i) {
      Result<Datum> value = evaluate(*statement.assignments[i].second, table->schema, before);
      if (!value.ok()) {
        return value.status();
      }
      const ColumnDef& column = table->schema.columns[static_cast<std::size_t>(targets[i])];
      Result<Datum> coerced = value.value().coerce(column.type);
      if (!coerced.ok()) {
        return Status::invalidArgument("column '" + column.name + "': " + coerced.status().message());
      }
      if (column.not_null && coerced.value().isNull()) {
        return Status::invalidArgument("column '" + column.name + "' is NOT NULL");
      }
      row[static_cast<std::size_t>(targets[i])] = std::move(coerced).value();
    }
    ++affected;
  }

  ResultSet result;
  result.rows_affected = affected;
  result.message = "updated " + std::to_string(affected) + " row(s)";
  return result;
}

Result<ResultSet> Engine::runDelete(const DeleteStatement& statement) {
  Table* table = findTable(statement.table);
  if (table == nullptr) {
    return Status::notFound("no such table: " + statement.table);
  }

  const std::size_t before = table->rows.size();
  Status failure;

  // erase-remove, with the predicate error captured rather than thrown: a
  // failing WHERE must not delete a partial set of rows.
  std::vector<Row> survivors;
  survivors.reserve(table->rows.size());
  for (Row& row : table->rows) {
    bool matched = true;
    if (statement.where) {
      Result<Datum> verdict = evaluate(*statement.where, table->schema, row);
      if (!verdict.ok()) {
        failure = verdict.status();
        break;
      }
      matched = verdict.value().isTrue();
    }
    if (!matched) {
      survivors.push_back(std::move(row));
    }
  }
  if (!failure.ok()) {
    return failure;
  }
  table->rows = std::move(survivors);

  ResultSet result;
  result.rows_affected = before - table->rows.size();
  result.message = "deleted " + std::to_string(result.rows_affected) + " row(s)";
  return result;
}

std::vector<std::string> Engine::tableNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(tables_.size());
  for (const auto& [key, table] : tables_) {
    names.push_back(table.schema.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool Engine::hasTable(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return findTable(name) != nullptr;
}

std::optional<TableSchema> Engine::schemaOf(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Table* table = findTable(name);
  if (table == nullptr) {
    return std::nullopt;
  }
  return table->schema;
}

std::size_t Engine::rowCount(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Table* table = findTable(name);
  return table == nullptr ? 0 : table->rows.size();
}

void Engine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  tables_.clear();
}

}  // namespace bourse::sql
