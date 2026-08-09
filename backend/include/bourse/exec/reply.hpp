#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bourse::exec {

/// A protocol-independent command result.
///
/// Commands return `Reply`, not bytes. That indirection is what lets the exact
/// same command implementations serve three transports: the RESP codec encodes
/// a Reply as RESP2, the REST layer renders it as JSON, and the test suite
/// inspects it structurally without parsing anything. Returning pre-encoded
/// RESP from commands -- the obvious shortcut -- would have welded the command
/// set to one wire format.
class Reply {
 public:
  enum class Kind : std::uint8_t {
    kSimpleString,  ///< +OK\r\n
    kError,         ///< -ERR ...\r\n
    kInteger,       ///< :42\r\n
    kBulkString,    ///< $3\r\nfoo\r\n
    kNull,          ///< $-1\r\n
    kArray,         ///< *2\r\n...
    kNullArray,     ///< *-1\r\n
  };

  Reply() : kind_(Kind::kNull) {}

  static Reply simpleString(std::string text) { return Reply(Kind::kSimpleString, std::move(text)); }

  static Reply error(std::string text) { return Reply(Kind::kError, std::move(text)); }

  static Reply bulkString(std::string text) { return Reply(Kind::kBulkString, std::move(text)); }

  static Reply null() { return Reply(Kind::kNull, {}); }

  static Reply nullArray() { return Reply(Kind::kNullArray, {}); }

  static Reply ok() { return simpleString("OK"); }

  static Reply integer(std::int64_t value) {
    Reply reply(Kind::kInteger, {});
    reply.integer_ = value;
    return reply;
  }

  static Reply array(std::vector<Reply> elements) {
    Reply reply(Kind::kArray, {});
    reply.elements_ = std::move(elements);
    return reply;
  }

  static Reply stringArray(const std::vector<std::string>& values) {
    std::vector<Reply> elements;
    elements.reserve(values.size());
    for (const std::string& value : values) {
      elements.push_back(bulkString(value));
    }
    return array(std::move(elements));
  }

  [[nodiscard]] Kind kind() const noexcept { return kind_; }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }

  [[nodiscard]] std::int64_t integerValue() const noexcept { return integer_; }

  [[nodiscard]] const std::vector<Reply>& elements() const noexcept { return elements_; }

  [[nodiscard]] bool isError() const noexcept { return kind_ == Kind::kError; }

  /// Appends the RESP2 encoding to `out`.
  void encodeResp(std::string& out) const;
  [[nodiscard]] std::string toResp() const;

  /// Appends a JSON rendering, used by the REST API and the dashboard.
  void encodeJson(std::string& out) const;
  [[nodiscard]] std::string toJson() const;

  /// Flat human-readable form, used by bourse-cli and by test failure output.
  [[nodiscard]] std::string toDisplayString() const;

 private:
  Reply(Kind kind, std::string text) : kind_(kind), text_(std::move(text)) {}

  Kind kind_;
  std::string text_;
  std::int64_t integer_ = 0;
  std::vector<Reply> elements_;
};

/// Escapes a string for embedding in JSON output.
void appendJsonEscaped(std::string& out, std::string_view text);

}  // namespace bourse::exec
