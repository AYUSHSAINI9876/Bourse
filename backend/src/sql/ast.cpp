#include "bourse/sql/ast.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace bourse::sql {
namespace {

std::string upperOf(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

const char* toString(DatumType type) noexcept {
  switch (type) {
    case DatumType::kNull: return "NULL";
    case DatumType::kInteger: return "INTEGER";
    case DatumType::kReal: return "REAL";
    case DatumType::kText: return "TEXT";
    case DatumType::kBoolean: return "BOOLEAN";
  }
  return "NULL";
}

bool parseDatumType(std::string_view text, DatumType& out) noexcept {
  const std::string upper = upperOf(text);
  if (upper == "INT" || upper == "INTEGER" || upper == "BIGINT") {
    out = DatumType::kInteger;
    return true;
  }
  if (upper == "REAL" || upper == "FLOAT" || upper == "DOUBLE") {
    out = DatumType::kReal;
    return true;
  }
  if (upper == "TEXT" || upper == "VARCHAR" || upper == "STRING" || upper == "CHAR") {
    out = DatumType::kText;
    return true;
  }
  if (upper == "BOOL" || upper == "BOOLEAN") {
    out = DatumType::kBoolean;
    return true;
  }
  return false;
}

const char* toString(BinaryOp op) noexcept {
  switch (op) {
    case BinaryOp::kAdd: return "+";
    case BinaryOp::kSubtract: return "-";
    case BinaryOp::kMultiply: return "*";
    case BinaryOp::kDivide: return "/";
    case BinaryOp::kModulo: return "%";
    case BinaryOp::kEqual: return "=";
    case BinaryOp::kNotEqual: return "!=";
    case BinaryOp::kLess: return "<";
    case BinaryOp::kLessEqual: return "<=";
    case BinaryOp::kGreater: return ">";
    case BinaryOp::kGreaterEqual: return ">=";
    case BinaryOp::kAnd: return "AND";
    case BinaryOp::kOr: return "OR";
    case BinaryOp::kLike: return "LIKE";
  }
  return "?";
}

const char* toString(UnaryOp op) noexcept {
  switch (op) {
    case UnaryOp::kNegate: return "-";
    case UnaryOp::kNot: return "NOT";
    case UnaryOp::kIsNull: return "IS NULL";
    case UnaryOp::kIsNotNull: return "IS NOT NULL";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Datum
// ---------------------------------------------------------------------------

DatumType Datum::type() const noexcept {
  switch (data_.index()) {
    case 0: return DatumType::kNull;
    case 1: return DatumType::kInteger;
    case 2: return DatumType::kReal;
    case 3: return DatumType::kText;
    case 4: return DatumType::kBoolean;
    default: return DatumType::kNull;
  }
}

std::int64_t Datum::asInteger() const {
  if (const auto* v = std::get_if<std::int64_t>(&data_)) {
    return *v;
  }
  if (const auto* v = std::get_if<double>(&data_)) {
    return static_cast<std::int64_t>(*v);
  }
  if (const auto* v = std::get_if<bool>(&data_)) {
    return *v ? 1 : 0;
  }
  return 0;
}

double Datum::asReal() const {
  if (const auto* v = std::get_if<double>(&data_)) {
    return *v;
  }
  if (const auto* v = std::get_if<std::int64_t>(&data_)) {
    return static_cast<double>(*v);
  }
  return 0.0;
}

const std::string& Datum::asText() const {
  static const std::string kEmpty;
  if (const auto* v = std::get_if<std::string>(&data_)) {
    return *v;
  }
  return kEmpty;
}

bool Datum::asBoolean() const {
  if (const auto* v = std::get_if<bool>(&data_)) {
    return *v;
  }
  return false;
}

std::string Datum::toString() const {
  switch (type()) {
    case DatumType::kNull: return {};
    case DatumType::kInteger: return std::to_string(std::get<std::int64_t>(data_));
    case DatumType::kBoolean: return std::get<bool>(data_) ? "true" : "false";
    case DatumType::kText: return std::get<std::string>(data_);
    case DatumType::kReal: {
      std::array<char, 40> buffer{};
      // %g rather than to_string: to_string always emits six decimals, so 1.5
      // would print as "1.500000" in every result set.
      std::snprintf(buffer.data(), buffer.size(), "%g", std::get<double>(data_));
      return std::string(buffer.data());
    }
  }
  return {};
}

std::string Datum::toJson() const {
  switch (type()) {
    case DatumType::kNull: return "null";
    case DatumType::kInteger: return std::to_string(std::get<std::int64_t>(data_));
    case DatumType::kBoolean: return std::get<bool>(data_) ? "true" : "false";
    case DatumType::kReal: return toString();
    case DatumType::kText: {
      std::string out;
      out.push_back('"');
      for (char c : std::get<std::string>(data_)) {
        switch (c) {
          case '"': out.append("\\\""); break;
          case '\\': out.append("\\\\"); break;
          case '\n': out.append("\\n"); break;
          case '\r': out.append("\\r"); break;
          case '\t': out.append("\\t"); break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {
              std::array<char, 8> escape{};
              std::snprintf(escape.data(), escape.size(), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
              out.append(escape.data());
            } else {
              out.push_back(c);
            }
        }
      }
      out.push_back('"');
      return out;
    }
  }
  return "null";
}

bool Datum::isTrue() const {
  switch (type()) {
    case DatumType::kNull: return false;  // NULL is unknown, never true
    case DatumType::kBoolean: return std::get<bool>(data_);
    case DatumType::kInteger: return std::get<std::int64_t>(data_) != 0;
    case DatumType::kReal: return std::get<double>(data_) != 0.0;
    case DatumType::kText: return !std::get<std::string>(data_).empty();
  }
  return false;
}

std::optional<int> Datum::compare(const Datum& a, const Datum& b) {
  // Any comparison involving NULL is unknown. This is what makes
  // `WHERE x = NULL` match nothing, which surprises people but is the standard.
  if (a.isNull() || b.isNull()) {
    return std::nullopt;
  }

  if (a.isNumeric() && b.isNumeric()) {
    if (a.type() == DatumType::kInteger && b.type() == DatumType::kInteger) {
      const std::int64_t lhs = a.asInteger();
      const std::int64_t rhs = b.asInteger();
      return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
    const double lhs = a.asReal();
    const double rhs = b.asReal();
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
  }

  if (a.type() == DatumType::kBoolean && b.type() == DatumType::kBoolean) {
    const int lhs = a.asBoolean() ? 1 : 0;
    const int rhs = b.asBoolean() ? 1 : 0;
    return lhs - rhs;
  }

  // Mixed or textual: compare as text. Loose, but predictable, and it keeps
  // `WHERE name = 'x'` working when the column was declared INTEGER by mistake.
  const std::string lhs = a.toString();
  const std::string rhs = b.toString();
  return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

int Datum::orderingCompare(const Datum& a, const Datum& b) {
  // ORDER BY needs a total order, so NULL has to land somewhere: first.
  if (a.isNull() && b.isNull()) {
    return 0;
  }
  if (a.isNull()) {
    return -1;
  }
  if (b.isNull()) {
    return 1;
  }
  return compare(a, b).value_or(0);
}

Result<Datum> Datum::coerce(DatumType target) const {
  if (isNull() || type() == target) {
    return *this;
  }

  switch (target) {
    case DatumType::kNull:
      return Datum::null();
    case DatumType::kText:
      return Datum::text(toString());
    case DatumType::kBoolean:
      return Datum::boolean(isTrue());
    case DatumType::kInteger: {
      if (isNumeric()) {
        return Datum::integer(asInteger());
      }
      const std::string& text = asText();
      std::int64_t parsed = 0;
      const char* begin = text.data();
      const char* end = begin + text.size();
      if (std::from_chars(begin, end, parsed).ec != std::errc{}) {
        return Status::invalidArgument("cannot store '" + text + "' in an INTEGER column");
      }
      return Datum::integer(parsed);
    }
    case DatumType::kReal: {
      if (isNumeric()) {
        return Datum::real(asReal());
      }
      const std::string& text = asText();
      double parsed = 0.0;
      const char* begin = text.data();
      const char* end = begin + text.size();
      if (std::from_chars(begin, end, parsed).ec != std::errc{}) {
        return Status::invalidArgument("cannot store '" + text + "' in a REAL column");
      }
      return Datum::real(parsed);
    }
  }
  return Status::internal("unreachable coercion target");
}

// ---------------------------------------------------------------------------
// Printing visitor
// ---------------------------------------------------------------------------

namespace {

/// The second visitor over the expression tree. Its existence is the argument
/// for the pattern: adding it required no change to any node class.
class PrintVisitor final : public ExpressionVisitor {
 public:
  void visit(const LiteralExpr& node) override {
    if (node.value().type() == DatumType::kText) {
      out_.push_back('\'');
      out_.append(node.value().asText());
      out_.push_back('\'');
    } else if (node.value().isNull()) {
      out_.append("NULL");
    } else {
      out_.append(node.value().toString());
    }
  }

  void visit(const ColumnExpr& node) override { out_.append(node.name()); }

  void visit(const BinaryExpr& node) override {
    out_.push_back('(');
    node.left().accept(*this);
    out_.push_back(' ');
    out_.append(sql::toString(node.op()));
    out_.push_back(' ');
    node.right().accept(*this);
    out_.push_back(')');
  }

  void visit(const UnaryExpr& node) override {
    if (node.op() == UnaryOp::kIsNull || node.op() == UnaryOp::kIsNotNull) {
      out_.push_back('(');
      node.operand().accept(*this);
      out_.push_back(' ');
      out_.append(sql::toString(node.op()));
      out_.push_back(')');
      return;
    }
    out_.push_back('(');
    out_.append(sql::toString(node.op()));
    out_.push_back(' ');
    node.operand().accept(*this);
    out_.push_back(')');
  }

  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

}  // namespace

std::string expressionToString(const Expression& expression) {
  PrintVisitor printer;
  expression.accept(printer);
  return printer.take();
}

}  // namespace bourse::sql
