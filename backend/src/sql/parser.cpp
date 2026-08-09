#include "bourse/sql/parser.hpp"

#include <charconv>
#include <utility>

namespace bourse::sql {

const Token& Parser::peek(std::size_t lookahead) const {
  const std::size_t index = std::min(cursor_ + lookahead, tokens_.size() - 1);
  return tokens_[index];
}

const Token& Parser::previous() const {
  return tokens_[cursor_ == 0 ? 0 : cursor_ - 1];
}

const Token& Parser::advance() {
  if (!atEnd()) {
    ++cursor_;
  }
  return previous();
}

bool Parser::atEnd() const {
  return peek().type == TokenType::kEndOfInput;
}

bool Parser::matchKeyword(std::string_view word) {
  if (peek().isKeyword(word)) {
    advance();
    return true;
  }
  return false;
}

bool Parser::matchPunctuation(std::string_view symbol) {
  if (peek().isPunctuation(symbol)) {
    advance();
    return true;
  }
  return false;
}

bool Parser::matchOperator(std::string_view symbol) {
  if (peek().isOperator(symbol)) {
    advance();
    return true;
  }
  return false;
}

Status Parser::errorHere(const std::string& message) const {
  const Token& token = peek();
  std::string found = token.type == TokenType::kEndOfInput ? "end of input" : "'" + token.text + "'";
  return Status::invalidArgument(message + ", found " + found + " at position " +
                                 std::to_string(token.position));
}

Status Parser::expectKeyword(std::string_view word) {
  if (matchKeyword(word)) {
    return Status::success();
  }
  return errorHere("expected " + std::string(word));
}

Status Parser::expectPunctuation(std::string_view symbol) {
  if (matchPunctuation(symbol)) {
    return Status::success();
  }
  return errorHere("expected '" + std::string(symbol) + "'");
}

Result<std::string> Parser::expectIdentifier(std::string_view what) {
  const Token& token = peek();
  // Non-reserved keywords are accepted as identifiers so a column called
  // "key" or "value" does not need quoting.
  if (token.type == TokenType::kIdentifier || token.type == TokenType::kKeyword) {
    advance();
    return token.text;
  }
  return errorHere("expected " + std::string(what));
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

Result<Statement> Parser::parse(std::string_view sql) {
  Lexer lexer(sql);
  Result<std::vector<Token>> tokens = lexer.tokenize();
  if (!tokens.ok()) {
    return tokens.status();
  }
  Parser parser(std::move(tokens).value());
  return parser.parseStatement();
}

Result<Statement> Parser::parseStatement() {
  if (atEnd()) {
    return Status::invalidArgument("empty statement");
  }

  Result<Statement> statement = Status::invalidArgument("unrecognised statement");
  if (peek().isKeyword("SELECT")) {
    statement = parseSelect();
  } else if (peek().isKeyword("INSERT")) {
    statement = parseInsert();
  } else if (peek().isKeyword("CREATE")) {
    statement = parseCreateTable();
  } else if (peek().isKeyword("DROP")) {
    statement = parseDropTable();
  } else if (peek().isKeyword("UPDATE")) {
    statement = parseUpdate();
  } else if (peek().isKeyword("DELETE")) {
    statement = parseDelete();
  } else {
    return errorHere("expected SELECT, INSERT, UPDATE, DELETE, CREATE or DROP");
  }

  if (!statement.ok()) {
    return statement;
  }
  (void)matchPunctuation(";");
  if (!atEnd()) {
    return errorHere("unexpected trailing input");
  }
  return statement;
}

Result<Statement> Parser::parseCreateTable() {
  BOURSE_TRY(expectKeyword("CREATE"));
  BOURSE_TRY(expectKeyword("TABLE"));

  CreateTableStatement statement;
  if (matchKeyword("IF")) {
    BOURSE_TRY(expectKeyword("NOT"));
    BOURSE_TRY(expectKeyword("EXISTS"));
    statement.if_not_exists = true;
  }

  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));
  BOURSE_TRY(expectPunctuation("("));

  do {
    ColumnDef column;
    BOURSE_ASSIGN_OR_RETURN(column.name, expectIdentifier("a column name"));

    const Token& type_token = peek();
    if (type_token.type != TokenType::kKeyword && type_token.type != TokenType::kIdentifier) {
      return errorHere("expected a column type");
    }
    if (!parseDatumType(type_token.text, column.type)) {
      return errorHere("unknown column type '" + type_token.text + "'");
    }
    advance();

    // Constraints, in any order.
    for (;;) {
      if (matchKeyword("PRIMARY")) {
        BOURSE_TRY(expectKeyword("KEY"));
        column.primary_key = true;
        column.not_null = true;  // a primary key is implicitly NOT NULL
        continue;
      }
      if (matchKeyword("NOT")) {
        BOURSE_TRY(expectKeyword("NULL"));
        column.not_null = true;
        continue;
      }
      break;
    }

    statement.columns.push_back(std::move(column));
  } while (matchPunctuation(","));

  BOURSE_TRY(expectPunctuation(")"));
  if (statement.columns.empty()) {
    return Status::invalidArgument("a table needs at least one column");
  }
  return Statement{std::move(statement)};
}

Result<Statement> Parser::parseDropTable() {
  BOURSE_TRY(expectKeyword("DROP"));
  BOURSE_TRY(expectKeyword("TABLE"));

  DropTableStatement statement;
  if (matchKeyword("IF")) {
    BOURSE_TRY(expectKeyword("EXISTS"));
    statement.if_exists = true;
  }
  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));
  return Statement{std::move(statement)};
}

Result<Statement> Parser::parseInsert() {
  BOURSE_TRY(expectKeyword("INSERT"));
  BOURSE_TRY(expectKeyword("INTO"));

  InsertStatement statement;
  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));

  if (matchPunctuation("(")) {
    do {
      BOURSE_ASSIGN_OR_RETURN(std::string column, expectIdentifier("a column name"));
      statement.columns.push_back(std::move(column));
    } while (matchPunctuation(","));
    BOURSE_TRY(expectPunctuation(")"));
  }

  BOURSE_TRY(expectKeyword("VALUES"));
  do {
    BOURSE_TRY(expectPunctuation("("));
    std::vector<ExpressionPtr> row;
    do {
      BOURSE_ASSIGN_OR_RETURN(ExpressionPtr value, parseExpression());
      row.push_back(std::move(value));
    } while (matchPunctuation(","));
    BOURSE_TRY(expectPunctuation(")"));
    statement.rows.push_back(std::move(row));
  } while (matchPunctuation(","));

  return Statement{std::move(statement)};
}

Result<Statement> Parser::parseSelect() {
  BOURSE_TRY(expectKeyword("SELECT"));

  SelectStatement statement;
  (void)matchKeyword("DISTINCT");  // accepted and ignored; documented in the README

  do {
    SelectItem item;
    if (matchOperator("*")) {
      item.expression = nullptr;  // wildcard
    } else {
      BOURSE_ASSIGN_OR_RETURN(item.expression, parseExpression());
      if (matchKeyword("AS")) {
        BOURSE_ASSIGN_OR_RETURN(item.alias, expectIdentifier("an alias"));
      }
    }
    statement.items.push_back(std::move(item));
  } while (matchPunctuation(","));

  BOURSE_TRY(expectKeyword("FROM"));
  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));

  if (matchKeyword("WHERE")) {
    BOURSE_ASSIGN_OR_RETURN(statement.where, parseExpression());
  }

  if (matchKeyword("ORDER")) {
    BOURSE_TRY(expectKeyword("BY"));
    do {
      OrderByItem item;
      BOURSE_ASSIGN_OR_RETURN(item.column, expectIdentifier("a column name"));
      if (matchKeyword("DESC")) {
        item.descending = true;
      } else {
        (void)matchKeyword("ASC");
      }
      statement.order_by.push_back(std::move(item));
    } while (matchPunctuation(","));
  }

  const auto parse_count = [&](std::int64_t& target, std::string_view what) -> Status {
    const Token& token = peek();
    if (token.type != TokenType::kNumber || token.is_real) {
      return errorHere("expected an integer after " + std::string(what));
    }
    std::int64_t value = 0;
    const char* begin = token.text.data();
    const char* end = begin + token.text.size();
    if (std::from_chars(begin, end, value).ec != std::errc{}) {
      return errorHere("malformed integer after " + std::string(what));
    }
    advance();
    target = value;
    return Status::success();
  };

  if (matchKeyword("LIMIT")) {
    BOURSE_TRY(parse_count(statement.limit, "LIMIT"));
  }
  if (matchKeyword("OFFSET")) {
    BOURSE_TRY(parse_count(statement.offset, "OFFSET"));
  }

  return Statement{std::move(statement)};
}

Result<Statement> Parser::parseUpdate() {
  BOURSE_TRY(expectKeyword("UPDATE"));

  UpdateStatement statement;
  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));
  BOURSE_TRY(expectKeyword("SET"));

  do {
    BOURSE_ASSIGN_OR_RETURN(std::string column, expectIdentifier("a column name"));
    if (!matchOperator("=")) {
      return errorHere("expected '=' in the assignment list");
    }
    BOURSE_ASSIGN_OR_RETURN(ExpressionPtr value, parseExpression());
    statement.assignments.emplace_back(std::move(column), std::move(value));
  } while (matchPunctuation(","));

  if (matchKeyword("WHERE")) {
    BOURSE_ASSIGN_OR_RETURN(statement.where, parseExpression());
  }
  return Statement{std::move(statement)};
}

Result<Statement> Parser::parseDelete() {
  BOURSE_TRY(expectKeyword("DELETE"));
  BOURSE_TRY(expectKeyword("FROM"));

  DeleteStatement statement;
  BOURSE_ASSIGN_OR_RETURN(statement.table, expectIdentifier("a table name"));
  if (matchKeyword("WHERE")) {
    BOURSE_ASSIGN_OR_RETURN(statement.where, parseExpression());
  }
  return Statement{std::move(statement)};
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

// SQL precedence, loosest first:
//     OR  <  AND  <  NOT  <  comparison / LIKE  <  + -  <  * / %
//
// NOT sitting *below* comparison is the part that is easy to get wrong. If NOT
// were treated as an ordinary tight-binding prefix operator (the C convention),
// `NOT a = b` would parse as `(NOT a) = b` and quietly return the wrong rows
// instead of failing. It is handled in parseExpression, not parseUnary, for
// exactly that reason.
namespace {
constexpr int kPrecedenceOr = 1;
constexpr int kPrecedenceAnd = 2;
constexpr int kPrecedenceNot = 3;
constexpr int kPrecedenceComparison = 4;
constexpr int kPrecedenceAdditive = 5;
constexpr int kPrecedenceMultiplicative = 6;
}  // namespace

int Parser::precedenceOf(const Token& token, BinaryOp& op) {
  if (token.type == TokenType::kKeyword) {
    if (token.upper == "OR") {
      op = BinaryOp::kOr;
      return kPrecedenceOr;
    }
    if (token.upper == "AND") {
      op = BinaryOp::kAnd;
      return kPrecedenceAnd;
    }
    if (token.upper == "LIKE") {
      op = BinaryOp::kLike;
      return kPrecedenceComparison;
    }
    return 0;
  }
  if (token.type != TokenType::kOperator) {
    return 0;
  }

  if (token.text == "=") {
    op = BinaryOp::kEqual;
    return kPrecedenceComparison;
  }
  if (token.text == "!=") {
    op = BinaryOp::kNotEqual;
    return kPrecedenceComparison;
  }
  if (token.text == "<") {
    op = BinaryOp::kLess;
    return kPrecedenceComparison;
  }
  if (token.text == "<=") {
    op = BinaryOp::kLessEqual;
    return kPrecedenceComparison;
  }
  if (token.text == ">") {
    op = BinaryOp::kGreater;
    return kPrecedenceComparison;
  }
  if (token.text == ">=") {
    op = BinaryOp::kGreaterEqual;
    return kPrecedenceComparison;
  }
  if (token.text == "+") {
    op = BinaryOp::kAdd;
    return kPrecedenceAdditive;
  }
  if (token.text == "-") {
    op = BinaryOp::kSubtract;
    return kPrecedenceAdditive;
  }
  if (token.text == "*") {
    op = BinaryOp::kMultiply;
    return kPrecedenceMultiplicative;
  }
  if (token.text == "/") {
    op = BinaryOp::kDivide;
    return kPrecedenceMultiplicative;
  }
  if (token.text == "%") {
    op = BinaryOp::kModulo;
    return kPrecedenceMultiplicative;
  }
  return 0;
}

Result<ExpressionPtr> Parser::parseExpression(int minimum_precedence) {
  ExpressionPtr left;

  if (peek().isKeyword("NOT") && minimum_precedence <= kPrecedenceNot) {
    advance();
    // The operand is parsed one level tighter, so NOT swallows the whole
    // comparison that follows it rather than just the next column reference.
    BOURSE_ASSIGN_OR_RETURN(ExpressionPtr operand, parseExpression(kPrecedenceNot + 1));
    left = std::make_unique<UnaryExpr>(UnaryOp::kNot, std::move(operand));
  } else {
    BOURSE_ASSIGN_OR_RETURN(left, parseUnary());
  }

  for (;;) {
    BinaryOp op{};
    const int precedence = precedenceOf(peek(), op);
    if (precedence == 0 || precedence < minimum_precedence) {
      break;
    }
    advance();
    // All operators here are left-associative, so the right operand binds one
    // level tighter.
    BOURSE_ASSIGN_OR_RETURN(ExpressionPtr right, parseExpression(precedence + 1));
    left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
  }
  return left;
}

Result<ExpressionPtr> Parser::parseUnary() {
  // NOT is deliberately absent here -- see the precedence note above.
  if (matchOperator("-")) {
    BOURSE_ASSIGN_OR_RETURN(ExpressionPtr operand, parseUnary());
    return ExpressionPtr{std::make_unique<UnaryExpr>(UnaryOp::kNegate, std::move(operand))};
  }
  BOURSE_ASSIGN_OR_RETURN(ExpressionPtr primary, parsePrimary());
  return parsePostfix(std::move(primary));
}

Result<ExpressionPtr> Parser::parsePostfix(ExpressionPtr operand) {
  // `IS NULL` / `IS NOT NULL` are postfix, so they are handled here rather than
  // as binary operators -- treating them as binary would let `a IS b` parse.
  if (matchKeyword("IS")) {
    const bool negated = matchKeyword("NOT");
    BOURSE_TRY(expectKeyword("NULL"));
    return ExpressionPtr{
        std::make_unique<UnaryExpr>(negated ? UnaryOp::kIsNotNull : UnaryOp::kIsNull, std::move(operand))};
  }
  return operand;
}

Result<ExpressionPtr> Parser::parsePrimary() {
  const Token& token = peek();

  if (matchPunctuation("(")) {
    BOURSE_ASSIGN_OR_RETURN(ExpressionPtr inner, parseExpression());
    BOURSE_TRY(expectPunctuation(")"));
    return inner;
  }

  if (token.type == TokenType::kNumber) {
    advance();
    if (token.is_real) {
      double value = 0.0;
      const char* begin = token.text.data();
      const char* end = begin + token.text.size();
      if (std::from_chars(begin, end, value).ec != std::errc{}) {
        return Status::invalidArgument("malformed numeric literal '" + token.text + "'");
      }
      return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::real(value))};
    }
    std::int64_t value = 0;
    const char* begin = token.text.data();
    const char* end = begin + token.text.size();
    if (std::from_chars(begin, end, value).ec != std::errc{}) {
      return Status::invalidArgument("integer literal out of range: '" + token.text + "'");
    }
    return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::integer(value))};
  }

  if (token.type == TokenType::kString) {
    advance();
    return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::text(token.text))};
  }

  if (token.isKeyword("NULL")) {
    advance();
    return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::null())};
  }
  if (token.isKeyword("TRUE")) {
    advance();
    return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::boolean(true))};
  }
  if (token.isKeyword("FALSE")) {
    advance();
    return ExpressionPtr{std::make_unique<LiteralExpr>(Datum::boolean(false))};
  }

  if (token.type == TokenType::kIdentifier) {
    advance();
    return ExpressionPtr{std::make_unique<ColumnExpr>(token.text)};
  }

  return errorHere("expected a value, column or '('");
}

}  // namespace bourse::sql
