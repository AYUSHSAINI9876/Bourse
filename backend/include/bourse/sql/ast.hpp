#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "bourse/core/result.hpp"

namespace bourse::sql {

enum class DatumType : std::uint8_t { kNull, kInteger, kReal, kText, kBoolean };

[[nodiscard]] const char* toString(DatumType type) noexcept;
[[nodiscard]] bool parseDatumType(std::string_view text, DatumType& out) noexcept;

/// A single SQL value.
///
/// NULL is a distinct alternative rather than a sentinel, because SQL's
/// three-valued logic is not expressible with in-band sentinels: `NULL = NULL`
/// is NULL, not true, and `WHERE x = NULL` matches nothing. Making it a type
/// forces every comparison site to decide what it means.
class Datum {
 public:
  Datum() = default;

  static Datum null() { return Datum{}; }

  static Datum integer(std::int64_t v) { return Datum(v); }

  static Datum real(double v) { return Datum(v); }

  static Datum text(std::string v) { return Datum(std::move(v)); }

  static Datum boolean(bool v) { return Datum(v); }

  [[nodiscard]] DatumType type() const noexcept;

  [[nodiscard]] bool isNull() const noexcept { return type() == DatumType::kNull; }

  [[nodiscard]] bool isNumeric() const noexcept {
    return type() == DatumType::kInteger || type() == DatumType::kReal;
  }

  [[nodiscard]] std::int64_t asInteger() const;
  [[nodiscard]] double asReal() const;
  [[nodiscard]] const std::string& asText() const;
  [[nodiscard]] bool asBoolean() const;

  /// Rendering for result sets. NULL renders as an empty cell, matching what
  /// most SQL shells do.
  [[nodiscard]] std::string toString() const;
  /// JSON rendering for the REST layer; NULL becomes a real JSON null.
  [[nodiscard]] std::string toJson() const;

  /// SQL truthiness: NULL is *not* true, which is what makes `WHERE` skip it.
  [[nodiscard]] bool isTrue() const;

  /// Three-way comparison. Returns nullopt when either side is NULL, which
  /// propagates SQL's unknown rather than inventing an ordering.
  [[nodiscard]] static std::optional<int> compare(const Datum& a, const Datum& b);
  /// Total order used by ORDER BY, where NULL must sort somewhere: nulls first.
  [[nodiscard]] static int orderingCompare(const Datum& a, const Datum& b);

  /// Coerces to `target`, failing rather than silently truncating.
  [[nodiscard]] Result<Datum> coerce(DatumType target) const;

 private:
  explicit Datum(std::int64_t v) : data_(v) {}

  explicit Datum(double v) : data_(v) {}

  explicit Datum(std::string v) : data_(std::move(v)) {}

  explicit Datum(bool v) : data_(v) {}

  std::variant<std::monostate, std::int64_t, double, std::string, bool> data_;
};

using Row = std::vector<Datum>;

// ---------------------------------------------------------------------------
// Expressions -- Composite, walked by Visitor
// ---------------------------------------------------------------------------

enum class BinaryOp : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kModulo,
  kEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kAnd,
  kOr,
  kLike,
};

enum class UnaryOp : std::uint8_t { kNegate, kNot, kIsNull, kIsNotNull };

[[nodiscard]] const char* toString(BinaryOp op) noexcept;
[[nodiscard]] const char* toString(UnaryOp op) noexcept;

class LiteralExpr;
class ColumnExpr;
class BinaryExpr;
class UnaryExpr;

/// Double-dispatch over the expression tree.
///
/// This is the textbook justification for Visitor: the set of node types is
/// closed and stable, while the set of *operations* over them keeps growing --
/// evaluate it, print it for EXPLAIN, collect the columns it references, check
/// its types. Putting each of those as a virtual method on every node would
/// mean editing four classes to add one operation. With a visitor, each new
/// operation is one new class and the nodes never change.
class ExpressionVisitor {
 public:
  virtual ~ExpressionVisitor() = default;
  virtual void visit(const LiteralExpr& node) = 0;
  virtual void visit(const ColumnExpr& node) = 0;
  virtual void visit(const BinaryExpr& node) = 0;
  virtual void visit(const UnaryExpr& node) = 0;
};

class Expression {
 public:
  virtual ~Expression() = default;
  virtual void accept(ExpressionVisitor& visitor) const = 0;
};

using ExpressionPtr = std::unique_ptr<Expression>;

class LiteralExpr final : public Expression {
 public:
  explicit LiteralExpr(Datum value) : value_(std::move(value)) {}

  void accept(ExpressionVisitor& visitor) const override { visitor.visit(*this); }

  [[nodiscard]] const Datum& value() const noexcept { return value_; }

 private:
  Datum value_;
};

class ColumnExpr final : public Expression {
 public:
  explicit ColumnExpr(std::string name) : name_(std::move(name)) {}

  void accept(ExpressionVisitor& visitor) const override { visitor.visit(*this); }

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  std::string name_;
};

class BinaryExpr final : public Expression {
 public:
  BinaryExpr(BinaryOp op, ExpressionPtr left, ExpressionPtr right)
      : op_(op), left_(std::move(left)), right_(std::move(right)) {}

  void accept(ExpressionVisitor& visitor) const override { visitor.visit(*this); }

  [[nodiscard]] BinaryOp op() const noexcept { return op_; }

  [[nodiscard]] const Expression& left() const noexcept { return *left_; }

  [[nodiscard]] const Expression& right() const noexcept { return *right_; }

 private:
  BinaryOp op_;
  ExpressionPtr left_;
  ExpressionPtr right_;
};

class UnaryExpr final : public Expression {
 public:
  UnaryExpr(UnaryOp op, ExpressionPtr operand) : op_(op), operand_(std::move(operand)) {}

  void accept(ExpressionVisitor& visitor) const override { visitor.visit(*this); }

  [[nodiscard]] UnaryOp op() const noexcept { return op_; }

  [[nodiscard]] const Expression& operand() const noexcept { return *operand_; }

 private:
  UnaryOp op_;
  ExpressionPtr operand_;
};

/// Renders an expression back to SQL-ish text. Used by EXPLAIN and by test
/// failure messages -- and it is the second visitor, which is the whole point.
[[nodiscard]] std::string expressionToString(const Expression& expression);

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

struct ColumnDef {
  std::string name;
  DatumType type = DatumType::kText;
  bool primary_key = false;
  bool not_null = false;
};

struct SelectItem {
  ExpressionPtr expression;  ///< null means `*`
  std::string alias;
};

struct OrderByItem {
  std::string column;
  bool descending = false;
};

struct CreateTableStatement {
  std::string table;
  std::vector<ColumnDef> columns;
  bool if_not_exists = false;
};

struct DropTableStatement {
  std::string table;
  bool if_exists = false;
};

struct InsertStatement {
  std::string table;
  std::vector<std::string> columns;  ///< empty means "all, in declaration order"
  std::vector<std::vector<ExpressionPtr>> rows;
};

struct SelectStatement {
  std::vector<SelectItem> items;
  std::string table;
  ExpressionPtr where;
  std::vector<OrderByItem> order_by;
  std::int64_t limit = -1;  ///< -1 = unlimited
  std::int64_t offset = 0;
};

struct UpdateStatement {
  std::string table;
  std::vector<std::pair<std::string, ExpressionPtr>> assignments;
  ExpressionPtr where;
};

struct DeleteStatement {
  std::string table;
  ExpressionPtr where;
};

using Statement = std::variant<CreateTableStatement, DropTableStatement, InsertStatement, SelectStatement,
                               UpdateStatement, DeleteStatement>;

}  // namespace bourse::sql
