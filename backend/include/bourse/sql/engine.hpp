#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bourse/sql/ast.hpp"

namespace bourse::sql {

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  std::size_t rows_affected = 0;
  std::string message;
  /// Human-readable plan tree, filled for SELECT. This is what `EXPLAIN` would
  /// print, and it is free because every plan node can already describe itself.
  std::string plan;

  [[nodiscard]] std::string toJson() const;
  [[nodiscard]] std::string toText() const;
};

struct TableSchema {
  std::string name;
  std::vector<ColumnDef> columns;

  /// Index of a column by name, or -1. Case-insensitive, because SQL is.
  [[nodiscard]] int indexOf(std::string_view column) const;
  [[nodiscard]] std::vector<std::string> columnNames() const;
};

struct Table {
  TableSchema schema;
  std::vector<Row> rows;
};

/// Volcano-model (a.k.a. iterator-model) plan node.
///
/// Every operator exposes the same three-call protocol and pulls one row at a
/// time from its child. That uniformity is the whole point: `LIMIT 10` over a
/// million-row table stops the scan after ten rows without any operator
/// knowing about any other, and a new operator can be inserted anywhere in the
/// tree without touching its neighbours.
///
/// The cost is one virtual call per row per operator, which is exactly why
/// modern engines moved to vectorised or compiled execution. At this scale the
/// clarity is worth more than the cycles, and that trade is worth being able to
/// state out loud.
class PlanNode {
 public:
  virtual ~PlanNode() = default;

  virtual void open() = 0;
  /// Returns false when the stream is exhausted *or* when an error occurred;
  /// callers must check `status()` afterwards to tell the two apart.
  virtual bool next(Row& out) = 0;
  virtual void close() = 0;

  [[nodiscard]] virtual std::string describe() const = 0;

  [[nodiscard]] virtual Status status() const { return Status::success(); }
};

using PlanNodePtr = std::unique_ptr<PlanNode>;

/// Evaluates an expression against one row. Implemented as an ExpressionVisitor.
[[nodiscard]] Result<Datum> evaluate(const Expression& expression, const TableSchema& schema, const Row& row);

/// SQL `LIKE`, with `%` and `_` wildcards.
[[nodiscard]] bool likeMatch(std::string_view pattern, std::string_view text);

PlanNodePtr makeSeqScan(const Table& table);
PlanNodePtr makeFilter(PlanNodePtr child, const Expression& predicate, const TableSchema& schema);
PlanNodePtr makeSort(PlanNodePtr child, std::vector<OrderByItem> keys, const TableSchema& schema);
PlanNodePtr makeLimit(PlanNodePtr child, std::int64_t limit, std::int64_t offset);

/// In-memory SQL engine over a catalog of tables.
///
/// The catalog is deliberately behind the same `Table` abstraction the storage
/// engine will implement, so swapping the row store for the on-disk B+ tree is
/// a change to one class rather than to the executor.
class Engine {
 public:
  [[nodiscard]] Result<ResultSet> execute(std::string_view sql);

  [[nodiscard]] std::vector<std::string> tableNames() const;
  [[nodiscard]] bool hasTable(const std::string& name) const;
  [[nodiscard]] std::optional<TableSchema> schemaOf(const std::string& name) const;
  [[nodiscard]] std::size_t rowCount(const std::string& name) const;
  void reset();

 private:
  [[nodiscard]] Result<ResultSet> runCreateTable(const CreateTableStatement& statement);
  [[nodiscard]] Result<ResultSet> runDropTable(const DropTableStatement& statement);
  [[nodiscard]] Result<ResultSet> runInsert(const InsertStatement& statement);
  [[nodiscard]] Result<ResultSet> runSelect(const SelectStatement& statement);
  [[nodiscard]] Result<ResultSet> runUpdate(const UpdateStatement& statement);
  [[nodiscard]] Result<ResultSet> runDelete(const DeleteStatement& statement);

  /// Caller must hold `mutex_`.
  [[nodiscard]] Table* findTable(const std::string& name);
  [[nodiscard]] const Table* findTable(const std::string& name) const;

  mutable std::mutex mutex_;
  std::map<std::string, Table, std::less<>> tables_;
};

}  // namespace bourse::sql
