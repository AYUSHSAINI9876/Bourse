#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/auth/principal.hpp"
#include "bourse/net/connection.hpp"

namespace bourse::net {

class Router;

/// Case-insensitive header map.
///
/// RFC 9110 makes field names case-insensitive, so a plain `unordered_map`
/// keyed by the bytes on the wire would miss `Content-Length` when the client
/// sent `content-length`. Storing them in a `std::map` with a case-insensitive
/// comparator makes that impossible to get wrong at the call site.
struct CaseInsensitiveLess {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept;
};

using HeaderMap = std::map<std::string, std::string, CaseInsensitiveLess>;

struct HttpRequest {
  std::string method;
  std::string target;   ///< raw request target, query string included
  std::string path;     ///< target with the query stripped, percent-decoded
  std::string version;  ///< "HTTP/1.1"
  HeaderMap headers;
  std::map<std::string, std::string> query;
  /// Populated by the router from `:name` path segments.
  std::map<std::string, std::string> params;
  std::string body;
  bool keep_alive = true;

  /// Resolved by the bearer-token middleware before any handler runs.
  ///
  /// HTTP is stateless, so unlike a RESP connection every request carries its
  /// own credentials and gets its own principal. Anonymous until proven
  /// otherwise -- the default is the one that grants nothing.
  auth::Principal principal;

  [[nodiscard]] std::string header(std::string_view name, std::string_view fallback = {}) const;
  [[nodiscard]] std::string queryParam(const std::string& name, std::string_view fallback = {}) const;
  [[nodiscard]] std::string pathParam(const std::string& name, std::string_view fallback = {}) const;
  [[nodiscard]] bool wantsWebSocketUpgrade() const;
};

class HttpResponse {
 public:
  int status = 200;
  HeaderMap headers;
  std::string body;

  static HttpResponse json(std::string payload, int status = 200);
  static HttpResponse text(std::string payload, int status = 200);
  static HttpResponse html(std::string payload, int status = 200);
  static HttpResponse error(int status, std::string message);
  static HttpResponse noContent();

  void setHeader(std::string name, std::string value) { headers[std::move(name)] = std::move(value); }

  /// Serialises status line, headers and body. `Content-Length` is always
  /// emitted -- omitting it forces the client to wait for a close to know the
  /// body ended, which breaks keep-alive.
  [[nodiscard]] std::string serialize(bool keep_alive) const;

  [[nodiscard]] static const char* reasonPhrase(int status) noexcept;
};

enum class HttpParseStatus : std::uint8_t {
  kIncomplete,
  kSuccess,
  kError,
};

struct HttpParseResult {
  HttpParseStatus status = HttpParseStatus::kIncomplete;
  HttpRequest request;
  std::size_t consumed = 0;
  int error_status = 400;
  std::string error;
};

/// Parses one HTTP/1.1 request from the front of `input`.
///
/// A free function over a `string_view` for the same reason the RESP parser is:
/// it can be driven a byte at a time by a test, and fuzzed without a socket.
/// Only `Content-Length` bodies are supported; a chunked request is rejected
/// with 411 rather than silently mis-parsed.
[[nodiscard]] HttpParseResult parseHttpRequest(std::string_view input);

[[nodiscard]] std::string percentDecode(std::string_view text);
[[nodiscard]] std::map<std::string, std::string> parseQueryString(std::string_view query);

/// Reads one top-level string field out of a JSON object.
///
/// Not a general JSON parser and not trying to be: the request bodies this
/// server accepts are flat objects of string fields, and a full parser would
/// be several hundred lines of attack surface for no gain. What it *does* do
/// properly matters, because it reads credentials:
///
///   - only top-level keys match, so `{"x":{"password":"a"},"password":"b"}`
///     returns `b` and never `a`;
///   - escape sequences are decoded, including `\uXXXX` with surrogate pairs,
///     so a password containing a quote survives the round trip;
///   - a non-string value reports "not found" rather than a coerced string;
///   - malformed input yields empty rather than a partial read.
///
/// `found` distinguishes an absent field from one that is present and empty.
[[nodiscard]] std::string jsonFieldOf(std::string_view json, std::string_view field, bool* found = nullptr);

/// HTTP/1.1 server codec with keep-alive.
class HttpCodec final : public ProtocolCodec {
 public:
  explicit HttpCodec(const Router& router) : router_(router) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "http"; }

  bool onData(Connection& connection, ByteBuffer& input) override;

  /// Requests larger than this are refused with 413 instead of being buffered.
  /// Without a cap, one client can make the server allocate without bound.
  static constexpr std::size_t kMaxRequestBytes = 8 * 1024 * 1024;
  static constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

 private:
  const Router& router_;
};

}  // namespace bourse::net
