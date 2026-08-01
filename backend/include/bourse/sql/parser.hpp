#pragma once

#include <string_view>
#include <vector>

#include "bourse/sql/ast.hpp"
#include "bourse/sql/lexer.hpp"

namespace bourse::sql {

/// Recursive-descent parser with precedence climbing for expressions.
///
/// Recursive descent, not a generator: the grammar is small, the code reads in
/// the same order as the grammar it implements, and errors can carry a position
/// and a "expected X, found Y" message. A parser generator would buy nothing
/// here except a build dependency and worse diagnostics.
///
/// Expression precedence is handled by precedence climbing rather than one
/// function per level. Six nested functions (`parseOr` calling `parseAnd`
/// calling `parseComparison`...) express the same thing with five extra call
/// frames and five more places to make a mistake.
class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  /// Parses exactly one statement, tolerating a trailing semicolon.
  [[nodiscard]] Result<Statement> parseStatement();

  /// Convenience: lex and parse in one step.
  [[nodiscard]] static Result<Statement> parse(std::string_view sql);

 private:
  [[nodiscard]] const Token& peek(std::size_t lookahead = 0) const;
  [[nodiscard]] const Token& previous() const;
  const Token& advance();
  [[nodiscard]] bool atEnd() const;

  bool matchKeyword(std::string_view word);
  bool matchPunctuation(std::string_view symbol);
  bool matchOperator(std::string_view symbol);

  [[nodiscard]] Status expectKeyword(std::string_view word);
  [[nodiscard]] Status expectPunctuation(std::string_view symbol);
  [[nodiscard]] Result<std::string> expectIdentifier(std::string_view what);
  [[nodiscard]] Status errorHere(const std::string& message) const;

  [[nodiscard]] Result<Statement> parseCreateTable();
  [[nodiscard]] Result<Statement> parseDropTable();
  [[nodiscard]] Result<Statement> parseInsert();
  [[nodiscard]] Result<Statement> parseSelect();
  [[nodiscard]] Result<Statement> parseUpdate();
  [[nodiscard]] Result<Statement> parseDelete();

  [[nodiscard]] Result<ExpressionPtr> parseExpression(int minimum_precedence = 0);
  [[nodiscard]] Result<ExpressionPtr> parseUnary();
  [[nodiscard]] Result<ExpressionPtr> parsePrimary();
  [[nodiscard]] Result<ExpressionPtr> parsePostfix(ExpressionPtr operand);

  /// Binding power of a binary operator; 0 means "not a binary operator".
  [[nodiscard]] static int precedenceOf(const Token& token, BinaryOp& op);

  std::vector<Token> tokens_;
  std::size_t cursor_ = 0;
};

}  // namespace bourse::sql
