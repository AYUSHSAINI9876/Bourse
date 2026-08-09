#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <variant>

/// \file result.hpp
/// Error handling for Bourse.
///
/// Design note: exceptions are used only for genuinely exceptional conditions
/// (bad_alloc, programmer error caught by assert). Every *expected* failure --
/// a missing key, a malformed command, a short read -- travels through
/// `Status` / `Result<T>`. This keeps the matching-engine hot path free of
/// unwinding tables and makes failure modes visible in every signature.

namespace bourse {

enum class ErrorCode : std::uint8_t {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kIoError,
  kCorruption,
  kUnsupported,
  kProtocolError,
  kWouldBlock,
  kClosed,
  kTimeout,
  kWrongType,
  kOutOfMemory,
  kInternal,
};

constexpr const char* toString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk: return "OK";
    case ErrorCode::kInvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::kNotFound: return "NOT_FOUND";
    case ErrorCode::kAlreadyExists: return "ALREADY_EXISTS";
    case ErrorCode::kIoError: return "IO_ERROR";
    case ErrorCode::kCorruption: return "CORRUPTION";
    case ErrorCode::kUnsupported: return "UNSUPPORTED";
    case ErrorCode::kProtocolError: return "PROTOCOL_ERROR";
    case ErrorCode::kWouldBlock: return "WOULD_BLOCK";
    case ErrorCode::kClosed: return "CLOSED";
    case ErrorCode::kTimeout: return "TIMEOUT";
    case ErrorCode::kWrongType: return "WRONG_TYPE";
    case ErrorCode::kOutOfMemory: return "OUT_OF_MEMORY";
    case ErrorCode::kInternal: return "INTERNAL";
  }
  return "UNKNOWN";
}

/// A movable, cheaply-default-constructed error value. The success case
/// allocates nothing.
class Status {
 public:
  Status() noexcept = default;

  Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  /// Named `success()` rather than `ok()` because the query method below owns
  /// that name; C++ will not overload a static factory against a const member.
  static Status success() noexcept { return Status{}; }

  static Status invalidArgument(std::string m) { return {ErrorCode::kInvalidArgument, std::move(m)}; }

  static Status notFound(std::string m) { return {ErrorCode::kNotFound, std::move(m)}; }

  static Status alreadyExists(std::string m) { return {ErrorCode::kAlreadyExists, std::move(m)}; }

  static Status ioError(std::string m) { return {ErrorCode::kIoError, std::move(m)}; }

  static Status corruption(std::string m) { return {ErrorCode::kCorruption, std::move(m)}; }

  static Status unsupported(std::string m) { return {ErrorCode::kUnsupported, std::move(m)}; }

  static Status protocolError(std::string m) { return {ErrorCode::kProtocolError, std::move(m)}; }

  static Status wrongType(std::string m) { return {ErrorCode::kWrongType, std::move(m)}; }

  static Status closed(std::string m) { return {ErrorCode::kClosed, std::move(m)}; }

  static Status internal(std::string m) { return {ErrorCode::kInternal, std::move(m)}; }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  [[nodiscard]] std::string toString() const {
    if (ok()) {
      return "OK";
    }
    return std::string(bourse::toString(code_)) + ": " + message_;
  }

  explicit operator bool() const noexcept { return ok(); }

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

/// `Result<T>` is either a value or a Status. Deliberately minimal -- it exists
/// because std::expected is C++23 and Bourse targets C++20.
template <typename T>
class Result {
 public:
  Result(T value) : slot_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

  Result(Status status) : slot_(std::move(status)) {  // NOLINT(google-explicit-constructor)
    assert(!std::get<Status>(slot_).ok() && "Result<T> constructed from an OK Status");
  }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(slot_); }

  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const {
    static const Status kOk;
    return ok() ? kOk : std::get<Status>(slot_);
  }

  T& value() & {
    assert(ok() && "Result<T>::value() on an error Result");
    return std::get<T>(slot_);
  }

  const T& value() const& {
    assert(ok() && "Result<T>::value() on an error Result");
    return std::get<T>(slot_);
  }

  T&& value() && {
    assert(ok() && "Result<T>::value() on an error Result");
    return std::get<T>(std::move(slot_));
  }

  T valueOr(T fallback) const& { return ok() ? std::get<T>(slot_) : std::move(fallback); }

  T* operator->() { return &value(); }

  const T* operator->() const { return &value(); }

  T& operator*() & { return value(); }

  const T& operator*() const& { return value(); }

 private:
  std::variant<T, Status> slot_;
};

/// Early-return helper. `BOURSE_TRY(status_expr);`
#define BOURSE_TRY(expr)                 \
  do {                                   \
    ::bourse::Status _bourse_s = (expr); \
    if (!_bourse_s.ok())                 \
      return _bourse_s;                  \
  } while (false)

/// Assign-or-return helper. `BOURSE_ASSIGN_OR_RETURN(auto v, mayFail());`
#define BOURSE_ASSIGN_OR_RETURN_IMPL(tmp, decl, expr) \
  auto tmp = (expr);                                  \
  if (!tmp.ok())                                      \
    return tmp.status();                              \
  decl = std::move(tmp).value()

#define BOURSE_CONCAT_INNER(a, b) a##b
#define BOURSE_CONCAT(a, b) BOURSE_CONCAT_INNER(a, b)
#define BOURSE_ASSIGN_OR_RETURN(decl, expr) \
  BOURSE_ASSIGN_OR_RETURN_IMPL(BOURSE_CONCAT(_bourse_tmp_, __LINE__), decl, expr)

}  // namespace bourse
