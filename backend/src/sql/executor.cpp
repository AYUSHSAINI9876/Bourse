#include <algorithm>
#include <cctype>
#include <utility>

#include "bourse/sql/engine.hpp"

namespace bourse::sql {
namespace {

/// Expression evaluation as a visitor.
///
/// The tree is walked with an explicit result slot rather than a return value
/// because `ExpressionVisitor::visit` cannot return one -- that is the standard
/// cost of double dispatch, and it is why `result_`/`error_` are members.
class Evaluator final : public ExpressionVisitor {
 public:
  Evaluator(const TableSchema& schema, const Row& row) : schema_(schema), row_(row) {}

  void visit(const LiteralExpr& node) override { result_ = node.value(); }

  void visit(const ColumnExpr& node) override {
    const int index = schema_.indexOf(node.name());
    if (index < 0) {
      fail("unknown column '" + node.name() + "'");
      return;
    }
    const auto slot = static_cast<std::size_t>(index);
    result_ = slot < row_.size() ? row_[slot] : Datum::null();
  }

  void visit(const UnaryExpr& node) override {
    node.operand().accept(*this);
    if (failed()) {
      return;
    }
    const Datum operand = result_;

    switch (node.op()) {
      case UnaryOp::kIsNull: result_ = Datum::boolean(operand.isNull()); return;
      case UnaryOp::kIsNotNull: result_ = Datum::boolean(!operand.isNull()); return;
      case UnaryOp::kNot:
        // NOT NULL is NULL, not true -- three-valued logic again.
        result_ = operand.isNull() ? Datum::null() : Datum::boolean(!operand.isTrue());
        return;
      case UnaryOp::kNegate:
        if (operand.isNull()) {
          result_ = Datum::null();
        } else if (operand.type() == DatumType::kReal) {
          result_ = Datum::real(-operand.asReal());
        } else if (operand.isNumeric()) {
          result_ = Datum::integer(-operand.asInteger());
        } else {
          fail("cannot negate a non-numeric value");
        }
        return;
    }
  }

  void visit(const BinaryExpr& node) override {
    // AND / OR short-circuit, so the right side is not evaluated unless needed.
    // Besides being faster, this is what stops `x IS NOT NULL AND x > 5` from
    // erroring on the rows where x is null.
    if (node.op() == BinaryOp::kAnd || node.op() == BinaryOp::kOr) {
      node.left().accept(*this);
      if (failed()) {
        return;
      }
      const Datum left = result_;
      if (node.op() == BinaryOp::kAnd && !left.isNull() && !left.isTrue()) {
        result_ = Datum::boolean(false);
        return;
      }
      if (node.op() == BinaryOp::kOr && left.isTrue()) {
        result_ = Datum::boolean(true);
        return;
      }
      node.right().accept(*this);
      if (failed()) {
        return;
      }
      const Datum right = result_;
      if (left.isNull() || right.isNull()) {
        result_ = Datum::null();
      } else {
        result_ = Datum::boolean(node.op() == BinaryOp::kAnd ? (left.isTrue() && right.isTrue())
                                                             : (left.isTrue() || right.isTrue()));
      }
      return;
    }

    node.left().accept(*this);
    if (failed()) {
      return;
    }
    const Datum left = result_;
    node.right().accept(*this);
    if (failed()) {
      return;
    }
    const Datum right = result_;

    if (node.op() == BinaryOp::kLike) {
      if (left.isNull() || right.isNull()) {
        result_ = Datum::null();
        return;
      }
      result_ = Datum::boolean(likeMatch(right.toString(), left.toString()));
      return;
    }

    switch (node.op()) {
      case BinaryOp::kEqual:
      case BinaryOp::kNotEqual:
      case BinaryOp::kLess:
      case BinaryOp::kLessEqual:
      case BinaryOp::kGreater:
      case BinaryOp::kGreaterEqual: {
        const std::optional<int> ordering = Datum::compare(left, right);
        if (!ordering.has_value()) {
          result_ = Datum::null();  // comparison with NULL is unknown
          return;
        }
        const int c = *ordering;
        bool value = false;
        switch (node.op()) {
          case BinaryOp::kEqual: value = c == 0; break;
          case BinaryOp::kNotEqual: value = c != 0; break;
          case BinaryOp::kLess: value = c < 0; break;
          case BinaryOp::kLessEqual: value = c <= 0; break;
          case BinaryOp::kGreater: value = c > 0; break;
          case BinaryOp::kGreaterEqual: value = c >= 0; break;
          default: break;
        }
        result_ = Datum::boolean(value);
        return;
      }
      default: break;
    }

    // Arithmetic.
    if (left.isNull() || right.isNull()) {
      result_ = Datum::null();
      return;
    }
    if (node.op() == BinaryOp::kAdd &&
        (left.type() == DatumType::kText || right.type() == DatumType::kText)) {
      result_ = Datum::text(left.toString() + right.toString());  // '+' concatenates text
      return;
    }
    if (!left.isNumeric() || !right.isNumeric()) {
      fail(std::string("operator ") + sql::toString(node.op()) + " needs numeric operands");
      return;
    }

    const bool real = left.type() == DatumType::kReal || right.type() == DatumType::kReal;
    if (real) {
      const double a = left.asReal();
      const double b = right.asReal();
      switch (node.op()) {
        case BinaryOp::kAdd: result_ = Datum::real(a + b); return;
        case BinaryOp::kSubtract: result_ = Datum::real(a - b); return;
        case BinaryOp::kMultiply: result_ = Datum::real(a * b); return;
        case BinaryOp::kDivide:
          if (b == 0.0) {
            fail("division by zero");
            return;
          }
          result_ = Datum::real(a / b);
          return;
        case BinaryOp::kModulo: fail("modulo needs integer operands"); return;
        default: break;
      }
    }

    const std::int64_t a = left.asInteger();
    const std::int64_t b = right.asInteger();
    switch (node.op()) {
      case BinaryOp::kAdd: result_ = Datum::integer(a + b); return;
      case BinaryOp::kSubtract: result_ = Datum::integer(a - b); return;
      case BinaryOp::kMultiply: result_ = Datum::integer(a * b); return;
      case BinaryOp::kDivide:
        if (b == 0) {
          fail("division by zero");
          return;
        }
        result_ = Datum::integer(a / b);
        return;
      case BinaryOp::kModulo:
        if (b == 0) {
          fail("division by zero");
          return;
        }
        result_ = Datum::integer(a % b);
        return;
      default: fail("unsupported operator"); return;
    }
  }

  [[nodiscard]] bool failed() const noexcept { return !error_.ok(); }

  [[nodiscard]] const Status& error() const noexcept { return error_; }

  [[nodiscard]] const Datum& result() const noexcept { return result_; }

 private:
  void fail(const std::string& message) {
    if (error_.ok()) {
      error_ = Status::invalidArgument(message);
    }
  }

  const TableSchema& schema_;
  const Row& row_;
  Datum result_;
  Status error_;
};

// ---------------------------------------------------------------------------
// Plan nodes
// ---------------------------------------------------------------------------

class SeqScanNode final : public PlanNode {
 public:
  explicit SeqScanNode(const Table& table) : table_(table) {}

  void open() override { cursor_ = 0; }

  void close() override { cursor_ = 0; }

  bool next(Row& out) override {
    if (cursor_ >= table_.rows.size()) {
      return false;
    }
    out = table_.rows[cursor_++];
    return true;
  }

  [[nodiscard]] std::string describe() const override {
    return "SeqScan(" + table_.schema.name + ", rows=" + std::to_string(table_.rows.size()) + ")";
  }

 private:
  const Table& table_;
  std::size_t cursor_ = 0;
};

class FilterNode final : public PlanNode {
 public:
  FilterNode(PlanNodePtr child, const Expression& predicate, const TableSchema& schema)
      : child_(std::move(child)), predicate_(predicate), schema_(schema) {}

  void open() override {
    child_->open();
    examined_ = 0;
    passed_ = 0;
  }

  void close() override { child_->close(); }

  bool next(Row& out) override {
    Row candidate;
    while (child_->next(candidate)) {
      ++examined_;
      Result<Datum> verdict = evaluate(predicate_, schema_, candidate);
      if (!verdict.ok()) {
        error_ = verdict.status();
        return false;
      }
      // A predicate that evaluates to NULL does not pass -- the row is not
      // known to match, so SQL excludes it.
      if (verdict.value().isTrue()) {
        ++passed_;
        out = std::move(candidate);
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string describe() const override {
    return "Filter(" + expressionToString(predicate_) + ", examined=" + std::to_string(examined_) +
           ", passed=" + std::to_string(passed_) + ")\n  " + child_->describe();
  }

  [[nodiscard]] Status status() const override { return error_.ok() ? child_->status() : error_; }

 private:
  PlanNodePtr child_;
  const Expression& predicate_;
  const TableSchema& schema_;
  Status error_;
  std::size_t examined_ = 0;
  std::size_t passed_ = 0;
};

class SortNode final : public PlanNode {
 public:
  SortNode(PlanNodePtr child, std::vector<OrderByItem> keys, const TableSchema& schema)
      : child_(std::move(child)), keys_(std::move(keys)), schema_(schema) {}

  void open() override {
    child_->open();
    buffer_.clear();
    cursor_ = 0;

    // Sort is a pipeline breaker: it must see every row before it can emit the
    // first, which is why it materialises. This is also why ORDER BY defeats
    // the early-exit that LIMIT would otherwise give a scan.
    Row row;
    while (child_->next(row)) {
      buffer_.push_back(std::move(row));
    }
    if (!child_->status().ok()) {
      error_ = child_->status();
      return;
    }

    std::vector<int> indices;
    indices.reserve(keys_.size());
    for (const OrderByItem& key : keys_) {
      const int index = schema_.indexOf(key.column);
      if (index < 0) {
        error_ = Status::invalidArgument("unknown column '" + key.column + "' in ORDER BY");
        return;
      }
      indices.push_back(index);
    }

    std::stable_sort(buffer_.begin(), buffer_.end(), [&](const Row& a, const Row& b) {
      for (std::size_t k = 0; k < indices.size(); ++k) {
        const auto slot = static_cast<std::size_t>(indices[k]);
        const Datum& lhs = slot < a.size() ? a[slot] : Datum::null();
        const Datum& rhs = slot < b.size() ? b[slot] : Datum::null();
        const int ordering = Datum::orderingCompare(lhs, rhs);
        if (ordering != 0) {
          return keys_[k].descending ? ordering > 0 : ordering < 0;
        }
      }
      return false;
    });
  }

  void close() override {
    child_->close();
    buffer_.clear();
  }

  bool next(Row& out) override {
    if (!error_.ok() || cursor_ >= buffer_.size()) {
      return false;
    }
    out = buffer_[cursor_++];
    return true;
  }

  [[nodiscard]] std::string describe() const override {
    std::string keys;
    for (const OrderByItem& key : keys_) {
      if (!keys.empty()) {
        keys.append(", ");
      }
      keys.append(key.column).append(key.descending ? " DESC" : " ASC");
    }
    return "Sort(" + keys + ", buffered=" + std::to_string(buffer_.size()) + ")\n  " + child_->describe();
  }

  [[nodiscard]] Status status() const override { return error_.ok() ? child_->status() : error_; }

 private:
  PlanNodePtr child_;
  std::vector<OrderByItem> keys_;
  const TableSchema& schema_;
  std::vector<Row> buffer_;
  std::size_t cursor_ = 0;
  Status error_;
};

class LimitNode final : public PlanNode {
 public:
  LimitNode(PlanNodePtr child, std::int64_t limit, std::int64_t offset)
      : child_(std::move(child)), limit_(limit), offset_(offset) {}

  void open() override {
    child_->open();
    emitted_ = 0;
    skipped_ = 0;
  }

  void close() override { child_->close(); }

  bool next(Row& out) override {
    if (limit_ >= 0 && emitted_ >= limit_) {
      // Stop pulling entirely. On a SeqScan this is what makes `LIMIT 10` over
      // a huge table cost ten rows rather than all of them.
      return false;
    }
    Row row;
    while (child_->next(row)) {
      if (skipped_ < offset_) {
        ++skipped_;
        continue;
      }
      ++emitted_;
      out = std::move(row);
      return true;
    }
    return false;
  }

  [[nodiscard]] std::string describe() const override {
    return "Limit(limit=" + (limit_ < 0 ? std::string("all") : std::to_string(limit_)) +
           ", offset=" + std::to_string(offset_) + ")\n  " + child_->describe();
  }

  [[nodiscard]] Status status() const override { return child_->status(); }

 private:
  PlanNodePtr child_;
  std::int64_t limit_;
  std::int64_t offset_;
  std::int64_t emitted_ = 0;
  std::int64_t skipped_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

Result<Datum> evaluate(const Expression& expression, const TableSchema& schema, const Row& row) {
  Evaluator evaluator(schema, row);
  expression.accept(evaluator);
  if (evaluator.failed()) {
    return evaluator.error();
  }
  return evaluator.result();
}

bool likeMatch(std::string_view pattern, std::string_view text) {
  // Iterative backtracking, same shape as the keyspace glob matcher: '%' is
  // the multi-character wildcard and '_' matches exactly one.
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_t = 0;

  auto same = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '_' || same(pattern[p], text[t]))) {
      ++p;
      ++t;
      continue;
    }
    if (p < pattern.size() && pattern[p] == '%') {
      star_p = p++;
      star_t = t;
      continue;
    }
    if (star_p != std::string_view::npos) {
      p = star_p + 1;
      t = ++star_t;
      continue;
    }
    return false;
  }
  while (p < pattern.size() && pattern[p] == '%') {
    ++p;
  }
  return p == pattern.size();
}

PlanNodePtr makeSeqScan(const Table& table) {
  return std::make_unique<SeqScanNode>(table);
}

PlanNodePtr makeFilter(PlanNodePtr child, const Expression& predicate, const TableSchema& schema) {
  return std::make_unique<FilterNode>(std::move(child), predicate, schema);
}

PlanNodePtr makeSort(PlanNodePtr child, std::vector<OrderByItem> keys, const TableSchema& schema) {
  return std::make_unique<SortNode>(std::move(child), std::move(keys), schema);
}

PlanNodePtr makeLimit(PlanNodePtr child, std::int64_t limit, std::int64_t offset) {
  return std::make_unique<LimitNode>(std::move(child), limit, offset);
}

}  // namespace bourse::sql
