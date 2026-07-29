#include "bourse/cache/value.hpp"

#include <charconv>
#include <numeric>

namespace bourse::cache {

const char* toString(ValueType type) noexcept {
  switch (type) {
    case ValueType::kNone: return "none";
    case ValueType::kString: return "string";
    case ValueType::kInteger: return "string";  // integer encoding is invisible to clients
    case ValueType::kList: return "list";
    case ValueType::kHash: return "hash";
    case ValueType::kSet: return "set";
  }
  return "none";
}

ValueType Value::type() const noexcept {
  switch (data_.index()) {
    case 0: return ValueType::kNone;
    case 1: return ValueType::kString;
    case 2: return ValueType::kInteger;
    case 3: return ValueType::kList;
    case 4: return ValueType::kHash;
    case 5: return ValueType::kSet;
    default: return ValueType::kNone;
  }
}

std::string Value::toStringValue() const {
  if (const auto* s = std::get_if<std::string>(&data_)) {
    return *s;
  }
  if (const auto* i = std::get_if<std::int64_t>(&data_)) {
    return std::to_string(*i);
  }
  return {};
}

Result<std::int64_t> Value::asInteger() const {
  if (const auto* i = std::get_if<std::int64_t>(&data_)) {
    return *i;
  }
  if (const auto* s = std::get_if<std::string>(&data_)) {
    return parseInteger(*s);
  }
  return Status::wrongType("value is not an integer or out of range");
}

std::size_t Value::cardinality() const noexcept {
  switch (type()) {
    case ValueType::kNone: return 0;
    case ValueType::kString:
    case ValueType::kInteger: return 1;
    case ValueType::kList: return std::get<ListValue>(data_).size();
    case ValueType::kHash: return std::get<HashValue>(data_).size();
    case ValueType::kSet: return std::get<SetValue>(data_).size();
  }
  return 0;
}

std::size_t Value::approximateBytes() const noexcept {
  // Node overheads are estimates, not measurements: libstdc++ hash nodes carry
  // a next pointer and a cached hash, and each std::string over the SSO limit
  // is a separate allocation. Exact accounting would need a custom allocator;
  // the eviction loop only needs relative magnitudes, so estimates suffice.
  constexpr std::size_t kNodeOverhead = 32;
  constexpr std::size_t kStringOverhead = sizeof(std::string);

  switch (type()) {
    case ValueType::kNone:
      return 0;
    case ValueType::kInteger:
      return sizeof(std::int64_t);
    case ValueType::kString:
      return kStringOverhead + std::get<std::string>(data_).capacity();
    case ValueType::kList: {
      const ListValue& list = std::get<ListValue>(data_);
      std::size_t total = sizeof(ListValue);
      for (const std::string& item : list) {
        total += kStringOverhead + item.capacity();
      }
      return total;
    }
    case ValueType::kHash: {
      const HashValue& hash = std::get<HashValue>(data_);
      std::size_t total = sizeof(HashValue);
      for (const auto& [field, item] : hash) {
        total += kNodeOverhead + 2 * kStringOverhead + field.capacity() + item.capacity();
      }
      return total;
    }
    case ValueType::kSet: {
      const SetValue& set = std::get<SetValue>(data_);
      std::size_t total = sizeof(SetValue);
      for (const std::string& member : set) {
        total += kNodeOverhead + kStringOverhead + member.capacity();
      }
      return total;
    }
  }
  return 0;
}

Result<std::int64_t> parseInteger(std::string_view text) {
  if (text.empty()) {
    return Status::invalidArgument("value is not an integer or out of range");
  }
  std::int64_t out = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, out);
  // Reject trailing junk explicitly: from_chars stops at the first invalid
  // character and reports success, so "12abc" would otherwise parse as 12.
  if (result.ec != std::errc{} || result.ptr != end) {
    return Status::invalidArgument("value is not an integer or out of range");
  }
  return out;
}

Result<double> parseDouble(std::string_view text) {
  if (text.empty()) {
    return Status::invalidArgument("value is not a valid float");
  }
  double out = 0.0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, out);
  if (result.ec != std::errc{} || result.ptr != end) {
    return Status::invalidArgument("value is not a valid float");
  }
  return out;
}

}  // namespace bourse::cache
