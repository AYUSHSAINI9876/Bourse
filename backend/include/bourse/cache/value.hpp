#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "bourse/core/result.hpp"

namespace bourse::cache {

using ListValue = std::deque<std::string>;
using HashValue = std::unordered_map<std::string, std::string>;
using SetValue = std::unordered_set<std::string>;

enum class ValueType : std::uint8_t {
  kNone = 0,
  kString,
  kInteger,
  kList,
  kHash,
  kSet,
};

[[nodiscard]] const char* toString(ValueType type) noexcept;

/// A dynamically typed keyspace value.
///
/// `std::variant` rather than an abstract base with virtual accessors: values
/// are created and destroyed at the request rate, and a variant keeps them in
/// one contiguous allocation with no vtable pointer and no separate heap node
/// per value. Type dispatch happens through `std::visit` or through the checked
/// accessors below, both of which the optimiser turns into a jump table.
///
/// `kInteger` exists as a distinct alternative from `kString` for the same
/// reason Redis has an int encoding: INCR on a string-encoded counter would
/// otherwise parse and re-serialise on every single call.
class Value {
 public:
  Value() = default;

  static Value makeString(std::string s) { return Value(std::move(s)); }

  static Value makeInteger(std::int64_t v) { return Value(v); }

  static Value makeList(ListValue v) { return Value(std::move(v)); }

  static Value makeHash(HashValue v) { return Value(std::move(v)); }

  static Value makeSet(SetValue v) { return Value(std::move(v)); }

  [[nodiscard]] ValueType type() const noexcept;

  [[nodiscard]] const char* typeName() const noexcept { return toString(type()); }

  [[nodiscard]] bool empty() const noexcept { return type() == ValueType::kNone; }

  [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }

  [[nodiscard]] bool isInteger() const noexcept { return std::holds_alternative<std::int64_t>(data_); }

  [[nodiscard]] bool isList() const noexcept { return std::holds_alternative<ListValue>(data_); }

  [[nodiscard]] bool isHash() const noexcept { return std::holds_alternative<HashValue>(data_); }

  [[nodiscard]] bool isSet() const noexcept { return std::holds_alternative<SetValue>(data_); }

  /// Both kString and kInteger answer to this -- an integer-encoded value is
  /// still a string as far as GET is concerned.
  [[nodiscard]] bool isStringLike() const noexcept { return isString() || isInteger(); }

  /// Materialises the value as text. Integers are formatted on demand.
  [[nodiscard]] std::string toStringValue() const;

  /// Parses to an integer if the value is integer-encoded or is a string that
  /// is exactly a base-10 integer with no leading/trailing junk.
  [[nodiscard]] Result<std::int64_t> asInteger() const;

  ListValue& list() { return std::get<ListValue>(data_); }

  const ListValue& list() const { return std::get<ListValue>(data_); }

  HashValue& hash() { return std::get<HashValue>(data_); }

  const HashValue& hash() const { return std::get<HashValue>(data_); }

  SetValue& set() { return std::get<SetValue>(data_); }

  const SetValue& set() const { return std::get<SetValue>(data_); }

  void setInteger(std::int64_t v) { data_ = v; }

  void setString(std::string s) { data_ = std::move(s); }

  /// Number of elements for containers, 1 for scalars, 0 for kNone. Used by
  /// the DEBUG/OBJECT introspection commands and the dashboard.
  [[nodiscard]] std::size_t cardinality() const noexcept;

  /// Approximate heap footprint, used by the maxmemory accounting. Deliberately
  /// an estimate: exact accounting would need an instrumented allocator, and
  /// the eviction loop only needs to know the relative cost of keys.
  [[nodiscard]] std::size_t approximateBytes() const noexcept;

 private:
  explicit Value(std::string s) : data_(std::move(s)) {}

  explicit Value(std::int64_t v) : data_(v) {}

  explicit Value(ListValue v) : data_(std::move(v)) {}

  explicit Value(HashValue v) : data_(std::move(v)) {}

  explicit Value(SetValue v) : data_(std::move(v)) {}

  std::variant<std::monostate, std::string, std::int64_t, ListValue, HashValue, SetValue> data_;
};

/// Parses a base-10 signed integer, rejecting partial matches ("12abc"),
/// empty input, and values outside int64 range.
[[nodiscard]] Result<std::int64_t> parseInteger(std::string_view text);

/// Parses a double for the SQL layer and for float-valued commands.
[[nodiscard]] Result<double> parseDouble(std::string_view text);

}  // namespace bourse::cache
