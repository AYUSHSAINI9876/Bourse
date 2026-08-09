#include "bourse/exec/reply.hpp"

#include <array>
#include <cstdio>

namespace bourse::exec {
namespace {
constexpr std::string_view kCrlf = "\r\n";
}

void appendJsonEscaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      default:
        // Control characters must be escaped as \u00XX or the output is not
        // valid JSON -- keys and values here are arbitrary client bytes.
        if (static_cast<unsigned char>(c) < 0x20) {
          std::array<char, 8> buffer{};
          std::snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
          out.append(buffer.data());
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void Reply::encodeResp(std::string& out) const {
  switch (kind_) {
    case Kind::kSimpleString:
      out.push_back('+');
      out.append(text_);
      out.append(kCrlf);
      break;
    case Kind::kError:
      out.push_back('-');
      out.append(text_);
      out.append(kCrlf);
      break;
    case Kind::kInteger:
      out.push_back(':');
      out.append(std::to_string(integer_));
      out.append(kCrlf);
      break;
    case Kind::kBulkString:
      out.push_back('$');
      out.append(std::to_string(text_.size()));
      out.append(kCrlf);
      out.append(text_);
      out.append(kCrlf);
      break;
    case Kind::kNull:
      out.append("$-1");
      out.append(kCrlf);
      break;
    case Kind::kNullArray:
      out.append("*-1");
      out.append(kCrlf);
      break;
    case Kind::kArray:
      out.push_back('*');
      out.append(std::to_string(elements_.size()));
      out.append(kCrlf);
      for (const Reply& element : elements_) {
        element.encodeResp(out);
      }
      break;
  }
}

std::string Reply::toResp() const {
  std::string out;
  encodeResp(out);
  return out;
}

void Reply::encodeJson(std::string& out) const {
  switch (kind_) {
    case Kind::kSimpleString:
    case Kind::kBulkString: appendJsonEscaped(out, text_); break;
    case Kind::kError:
      out.append("{\"error\":");
      appendJsonEscaped(out, text_);
      out.push_back('}');
      break;
    case Kind::kInteger: out.append(std::to_string(integer_)); break;
    case Kind::kNull:
    case Kind::kNullArray: out.append("null"); break;
    case Kind::kArray:
      out.push_back('[');
      for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        elements_[i].encodeJson(out);
      }
      out.push_back(']');
      break;
  }
}

std::string Reply::toJson() const {
  std::string out;
  encodeJson(out);
  return out;
}

std::string Reply::toDisplayString() const {
  switch (kind_) {
    case Kind::kSimpleString: return text_;
    case Kind::kError: return "(error) " + text_;
    case Kind::kInteger: return "(integer) " + std::to_string(integer_);
    case Kind::kBulkString: return "\"" + text_ + "\"";
    case Kind::kNull: return "(nil)";
    case Kind::kNullArray: return "(nil)";
    case Kind::kArray: {
      if (elements_.empty()) {
        return "(empty array)";
      }
      std::string out;
      for (std::size_t i = 0; i < elements_.size(); ++i) {
        out.append(std::to_string(i + 1));
        out.append(") ");
        out.append(elements_[i].toDisplayString());
        if (i + 1 < elements_.size()) {
          out.push_back('\n');
        }
      }
      return out;
    }
  }
  return {};
}

}  // namespace bourse::exec
