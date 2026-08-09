#include "bourse/core/system_error.hpp"

#include <cstring>
#include <string>
#include <type_traits>

#include "bourse/core/common.hpp"

#if defined(BOURSE_PLATFORM_WINDOWS)
#include <windows.h>
#endif

namespace bourse {

std::string describeSystemError(int code) {
#if defined(BOURSE_PLATFORM_WINDOWS)
  char* text = nullptr;
  ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&text), 0, nullptr);
  std::string message = text != nullptr ? text : "unknown";
  if (text != nullptr) {
    ::LocalFree(text);
  }
  // FormatMessage appends CRLF; a trailing newline inside a log line splits it.
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
#else
  char buffer[256];

  // strerror_r has two incompatible signatures. glibc's default (_GNU_SOURCE,
  // which libstdc++ defines) returns `char*` and may ignore the buffer
  // entirely, returning a pointer to an immutable static string; the POSIX/XSI
  // one returns `int` and always fills the buffer. Using the wrong one
  // compiles on the other and produces garbage, so both are handled by type
  // rather than by guessing from feature macros.
  //
  // `if constexpr` over the return type is what makes this a compile-time
  // choice with no runtime cost and no preprocessor guessing.
  using ReturnType = decltype(::strerror_r(0, buffer, sizeof(buffer)));
  if constexpr (std::is_same_v<ReturnType, char*>) {
    // GNU: the return value is the message, which may or may not be `buffer`.
    return std::string(::strerror_r(code, buffer, sizeof(buffer)));
  } else {
    // XSI: returns 0 on success and fills `buffer`.
    if (::strerror_r(code, buffer, sizeof(buffer)) == 0) {
      return std::string(buffer);
    }
    return "unknown error " + std::to_string(code);
  }
#endif
}

}  // namespace bourse
