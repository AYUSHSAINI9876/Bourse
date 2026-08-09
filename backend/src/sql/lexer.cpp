#include "bourse/sql/lexer.hpp"

#include <array>
#include <cctype>
#include <unordered_set>

namespace bourse::sql {
namespace {

const std::unordered_set<std::string>& keywordSet() {
  static const std::unordered_set<std::string> keywords = {
      "SELECT",  "FROM",  "WHERE", "INSERT", "INTO",     "VALUES",  "UPDATE",  "SET",    "DELETE",
      "CREATE",  "TABLE", "DROP",  "IF",     "NOT",      "EXISTS",  "AND",     "OR",     "NULL",
      "IS",      "ORDER", "BY",    "ASC",    "DESC",     "LIMIT",   "OFFSET",  "AS",     "PRIMARY",
      "KEY",     "TRUE",  "FALSE", "LIKE",   "DISTINCT", "INT",     "INTEGER", "BIGINT", "TEXT",
      "VARCHAR", "REAL",  "FLOAT", "DOUBLE", "BOOL",     "BOOLEAN", "CHAR",    "STRING",
  };
  return keywords;
}

std::string upperOf(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

Status errorAt(std::size_t position, const std::string& message) {
  return Status::invalidArgument(message + " at position " + std::to_string(position));
}

}  // namespace

bool Lexer::isKeyword(std::string_view upper) noexcept {
  return keywordSet().find(std::string(upper)) != keywordSet().end();
}

char Lexer::peek(std::size_t lookahead) const noexcept {
  const std::size_t index = cursor_ + lookahead;
  return index < input_.size() ? input_[index] : '\0';
}

void Lexer::skipWhitespaceAndComments() {
  for (;;) {
    while (cursor_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[cursor_])) != 0) {
      ++cursor_;
    }
    if (peek() == '-' && peek(1) == '-') {
      while (cursor_ < input_.size() && input_[cursor_] != '\n') {
        ++cursor_;
      }
      continue;
    }
    if (peek() == '/' && peek(1) == '*') {
      cursor_ += 2;
      while (cursor_ < input_.size() && !(peek() == '*' && peek(1) == '/')) {
        ++cursor_;
      }
      cursor_ = std::min(cursor_ + 2, input_.size());
      continue;
    }
    return;
  }
}

Result<Token> Lexer::scanString(char quote) {
  const std::size_t start = cursor_;
  ++cursor_;  // opening quote

  std::string value;
  while (cursor_ < input_.size()) {
    const char c = input_[cursor_];
    if (c == quote) {
      // SQL escapes a quote by doubling it: 'it''s'.
      if (peek(1) == quote) {
        value.push_back(quote);
        cursor_ += 2;
        continue;
      }
      ++cursor_;
      Token token;
      token.type = TokenType::kString;
      token.text = std::move(value);
      token.position = start;
      return token;
    }
    value.push_back(c);
    ++cursor_;
  }
  return errorAt(start, "unterminated string literal");
}

Result<Token> Lexer::scanNumber() {
  const std::size_t start = cursor_;
  bool seen_dot = false;

  while (cursor_ < input_.size()) {
    const char c = input_[cursor_];
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      ++cursor_;
      continue;
    }
    if (c == '.' && !seen_dot) {
      seen_dot = true;
      ++cursor_;
      continue;
    }
    break;
  }

  Token token;
  token.type = TokenType::kNumber;
  token.text = std::string(input_.substr(start, cursor_ - start));
  token.upper = token.text;
  token.position = start;
  token.is_real = seen_dot;
  if (token.text == ".") {
    return errorAt(start, "malformed numeric literal");
  }
  return token;
}

Token Lexer::scanIdentifierOrKeyword() {
  const std::size_t start = cursor_;
  while (cursor_ < input_.size()) {
    const char c = input_[cursor_];
    if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == ':') {
      ++cursor_;
      continue;
    }
    break;
  }

  Token token;
  token.text = std::string(input_.substr(start, cursor_ - start));
  token.upper = upperOf(token.text);
  token.position = start;
  token.type = isKeyword(token.upper) ? TokenType::kKeyword : TokenType::kIdentifier;
  return token;
}

Result<Token> Lexer::scanOperatorOrPunctuation() {
  const std::size_t start = cursor_;
  const char c = input_[cursor_];

  // Two-character operators must be tried first, or "<=" lexes as "<" then "=".
  static constexpr std::array<std::string_view, 4> kTwoCharOperators = {"<=", ">=", "!=", "<>"};
  if (cursor_ + 1 < input_.size()) {
    const std::string_view pair = input_.substr(cursor_, 2);
    for (std::string_view candidate : kTwoCharOperators) {
      if (pair == candidate) {
        cursor_ += 2;
        Token token;
        token.type = TokenType::kOperator;
        token.text = candidate == "<>" ? "!=" : std::string(candidate);
        token.position = start;
        return token;
      }
    }
  }

  static constexpr std::string_view kSingleOperators = "=<>+-*/%";
  static constexpr std::string_view kPunctuation = "(),;";

  if (kSingleOperators.find(c) != std::string_view::npos) {
    ++cursor_;
    Token token;
    token.type = TokenType::kOperator;
    token.text = std::string(1, c);
    token.position = start;
    return token;
  }
  if (kPunctuation.find(c) != std::string_view::npos) {
    ++cursor_;
    Token token;
    token.type = TokenType::kPunctuation;
    token.text = std::string(1, c);
    token.position = start;
    return token;
  }

  return errorAt(start, std::string("unexpected character '") + c + "'");
}

Result<std::vector<Token>> Lexer::tokenize() {
  std::vector<Token> tokens;

  for (;;) {
    skipWhitespaceAndComments();
    if (cursor_ >= input_.size()) {
      break;
    }

    const char c = input_[cursor_];
    if (c == '\'' || c == '"') {
      Result<Token> token = scanString(c);
      if (!token.ok()) {
        return token.status();
      }
      tokens.push_back(std::move(token).value());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      Result<Token> token = scanNumber();
      if (!token.ok()) {
        return token.status();
      }
      tokens.push_back(std::move(token).value());
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
      tokens.push_back(scanIdentifierOrKeyword());
      continue;
    }
    if (c == '*') {
      // '*' is both the SELECT wildcard and multiplication; the parser decides
      // by position, so the lexer just hands it over as an operator.
      ++cursor_;
      Token token;
      token.type = TokenType::kOperator;
      token.text = "*";
      token.position = cursor_ - 1;
      tokens.push_back(token);
      continue;
    }

    Result<Token> token = scanOperatorOrPunctuation();
    if (!token.ok()) {
      return token.status();
    }
    tokens.push_back(std::move(token).value());
  }

  Token end;
  end.type = TokenType::kEndOfInput;
  end.position = input_.size();
  tokens.push_back(end);
  return tokens;
}

}  // namespace bourse::sql
