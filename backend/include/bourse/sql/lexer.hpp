#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/core/result.hpp"

namespace bourse::sql {

enum class TokenType : std::uint8_t {
  kEndOfInput,
  kIdentifier,
  kNumber,
  kString,
  kKeyword,
  kOperator,
  kPunctuation,
};

struct Token {
  TokenType type = TokenType::kEndOfInput;
  std::string text;   ///< identifiers are stored as written
  std::string upper;  ///< uppercased, for keyword comparison
  std::size_t position = 0;
  bool is_real = false;  ///< numeric literal contained a '.'

  [[nodiscard]] bool isKeyword(std::string_view word) const noexcept {
    return type == TokenType::kKeyword && upper == word;
  }

  [[nodiscard]] bool isPunctuation(std::string_view symbol) const noexcept {
    return type == TokenType::kPunctuation && text == symbol;
  }

  [[nodiscard]] bool isOperator(std::string_view symbol) const noexcept {
    return type == TokenType::kOperator && text == symbol;
  }
};

/// Hand-written scanner.
///
/// A regex-based lexer is shorter to write and much harder to debug: it gives
/// no position on failure and hides the string-literal escaping rules. This one
/// tracks byte offsets so a parse error can point at the offending character,
/// which is the difference between a usable error message and "syntax error".
class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  /// Scans the whole input. Returns an error positioned at the offending byte
  /// for unterminated strings and unknown characters.
  [[nodiscard]] Result<std::vector<Token>> tokenize();

  [[nodiscard]] static bool isKeyword(std::string_view upper) noexcept;

 private:
  [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;
  void skipWhitespaceAndComments();
  [[nodiscard]] Result<Token> scanString(char quote);
  [[nodiscard]] Result<Token> scanNumber();
  [[nodiscard]] Token scanIdentifierOrKeyword();
  [[nodiscard]] Result<Token> scanOperatorOrPunctuation();

  std::string_view input_;
  std::size_t cursor_ = 0;
};

}  // namespace bourse::sql
