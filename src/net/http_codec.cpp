#include "bourse/net/http_codec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "bourse/core/logger.hpp"
#include "bourse/net/router.hpp"

namespace bourse::net {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kHeaderTerminator = "\r\n\r\n";

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string lower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool CaseInsensitiveLess::operator()(std::string_view a, std::string_view b) const noexcept {
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto lhs = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
    const auto rhs = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (lhs != rhs) {
      return lhs < rhs;
    }
  }
  return a.size() < b.size();
}

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

std::string HttpRequest::header(std::string_view name, std::string_view fallback) const {
  auto it = headers.find(name);
  return it == headers.end() ? std::string(fallback) : it->second;
}

std::string HttpRequest::queryParam(const std::string& name, std::string_view fallback) const {
  auto it = query.find(name);
  return it == query.end() ? std::string(fallback) : it->second;
}

std::string HttpRequest::pathParam(const std::string& name, std::string_view fallback) const {
  auto it = params.find(name);
  return it == params.end() ? std::string(fallback) : it->second;
}

bool HttpRequest::wantsWebSocketUpgrade() const {
  return lower(header("Upgrade")) == "websocket" && lower(header("Connection")).find("upgrade") != std::string::npos;
}

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

const char* HttpResponse::reasonPhrase(int status) noexcept {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
  }
}

HttpResponse HttpResponse::json(std::string payload, int status) {
  HttpResponse response;
  response.status = status;
  response.body = std::move(payload);
  response.setHeader("Content-Type", "application/json; charset=utf-8");
  return response;
}

HttpResponse HttpResponse::text(std::string payload, int status) {
  HttpResponse response;
  response.status = status;
  response.body = std::move(payload);
  response.setHeader("Content-Type", "text/plain; charset=utf-8");
  return response;
}

HttpResponse HttpResponse::html(std::string payload, int status) {
  HttpResponse response;
  response.status = status;
  response.body = std::move(payload);
  response.setHeader("Content-Type", "text/html; charset=utf-8");
  return response;
}

HttpResponse HttpResponse::error(int status, std::string message) {
  std::string payload = "{\"error\":\"";
  for (char c : message) {
    if (c == '"' || c == '\\') {
      payload.push_back('\\');
    }
    payload.push_back(c);
  }
  payload.append("\",\"status\":").append(std::to_string(status)).push_back('}');
  return json(std::move(payload), status);
}

HttpResponse HttpResponse::noContent() {
  HttpResponse response;
  response.status = 204;
  return response;
}

std::string HttpResponse::serialize(bool keep_alive) const {
  std::string out;
  out.reserve(body.size() + 256);

  out.append("HTTP/1.1 ").append(std::to_string(status)).push_back(' ');
  out.append(reasonPhrase(status)).append(kCrlf);

  for (const auto& [name, value] : headers) {
    out.append(name).append(": ").append(value).append(kCrlf);
  }

  // Always present. Without Content-Length the client cannot tell where the
  // body ends without waiting for a close, which defeats keep-alive entirely.
  if (headers.find("Content-Length") == headers.end()) {
    out.append("Content-Length: ").append(std::to_string(body.size())).append(kCrlf);
  }
  out.append("Connection: ").append(keep_alive ? "keep-alive" : "close").append(kCrlf);
  out.append("Server: bourse/1.0").append(kCrlf);
  out.append(kCrlf);
  out.append(body);
  return out;
}

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------

std::string percentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hexDigit(text[i + 1]);
      const int low = hexDigit(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::map<std::string, std::string> parseQueryString(std::string_view query) {
  std::map<std::string, std::string> out;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    std::string_view pair = amp == std::string_view::npos ? query : query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    if (pair.empty()) {
      continue;
    }
    const std::size_t eq = pair.find('=');
    if (eq == std::string_view::npos) {
      out.emplace(percentDecode(pair), std::string{});
    } else {
      out.insert_or_assign(percentDecode(pair.substr(0, eq)), percentDecode(pair.substr(eq + 1)));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

HttpParseResult parseHttpRequest(std::string_view input) {
  HttpParseResult out;
  if (input.empty()) {
    return out;
  }

  const std::size_t header_end = input.find(kHeaderTerminator);
  if (header_end == std::string_view::npos) {
    if (input.size() > HttpCodec::kMaxHeaderBytes) {
      out.status = HttpParseStatus::kError;
      out.error_status = 431;
      out.error = "header block too large";
    }
    return out;  // incomplete
  }

  std::string_view head = input.substr(0, header_end);

  // ---- request line ------------------------------------------------------
  const std::size_t line_end = head.find(kCrlf);
  std::string_view request_line = line_end == std::string_view::npos ? head : head.substr(0, line_end);

  const std::size_t first_space = request_line.find(' ');
  if (first_space == std::string_view::npos) {
    out.status = HttpParseStatus::kError;
    out.error = "malformed request line";
    return out;
  }
  const std::size_t second_space = request_line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) {
    out.status = HttpParseStatus::kError;
    out.error = "malformed request line";
    return out;
  }

  out.request.method = std::string(request_line.substr(0, first_space));
  out.request.target = std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
  out.request.version = std::string(trim(request_line.substr(second_space + 1)));

  if (out.request.version != "HTTP/1.1" && out.request.version != "HTTP/1.0") {
    out.status = HttpParseStatus::kError;
    out.error_status = 505;
    out.error = "unsupported HTTP version";
    return out;
  }

  // Split target into path and query, then percent-decode the path only. The
  // query must be split on & and = *before* decoding, or a %26 inside a value
  // would be mistaken for a separator.
  const std::size_t question = out.request.target.find('?');
  if (question == std::string::npos) {
    out.request.path = percentDecode(out.request.target);
  } else {
    out.request.path = percentDecode(std::string_view(out.request.target).substr(0, question));
    out.request.query = parseQueryString(std::string_view(out.request.target).substr(question + 1));
  }

  // ---- headers -----------------------------------------------------------
  std::string_view rest = line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
  while (!rest.empty()) {
    const std::size_t eol = rest.find(kCrlf);
    std::string_view line = eol == std::string_view::npos ? rest : rest.substr(0, eol);
    rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 2);
    if (line.empty()) {
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      out.status = HttpParseStatus::kError;
      out.error = "malformed header line";
      return out;
    }
    out.request.headers.insert_or_assign(std::string(trim(line.substr(0, colon))),
                                         std::string(trim(line.substr(colon + 1))));
  }

  // ---- body --------------------------------------------------------------
  const std::size_t body_start = header_end + kHeaderTerminator.size();

  if (!out.request.header("Transfer-Encoding").empty()) {
    // Chunked bodies are rejected outright rather than mis-parsed. A parser
    // that silently ignores Transfer-Encoding is a request-smuggling bug.
    out.status = HttpParseStatus::kError;
    out.error_status = 411;
    out.error = "Transfer-Encoding is not supported; use Content-Length";
    return out;
  }

  std::size_t content_length = 0;
  const std::string length_header = out.request.header("Content-Length");
  if (!length_header.empty()) {
    const char* begin = length_header.data();
    const char* end = begin + length_header.size();
    if (std::from_chars(begin, end, content_length).ec != std::errc{}) {
      out.status = HttpParseStatus::kError;
      out.error = "invalid Content-Length";
      return out;
    }
    if (content_length > HttpCodec::kMaxRequestBytes) {
      out.status = HttpParseStatus::kError;
      out.error_status = 413;
      out.error = "request body too large";
      return out;
    }
  }

  if (input.size() - body_start < content_length) {
    return HttpParseResult{};  // incomplete -- wait for the rest of the body
  }
  out.request.body = std::string(input.substr(body_start, content_length));

  // HTTP/1.1 is keep-alive by default, HTTP/1.0 is not; an explicit
  // Connection header overrides either way.
  const std::string connection = lower(out.request.header("Connection"));
  out.request.keep_alive = out.request.version == "HTTP/1.1";
  if (connection == "close") {
    out.request.keep_alive = false;
  } else if (connection.find("keep-alive") != std::string::npos) {
    out.request.keep_alive = true;
  }

  out.consumed = body_start + content_length;
  out.status = HttpParseStatus::kSuccess;
  return out;
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

bool HttpCodec::onData(Connection& connection, ByteBuffer& input) {
  for (;;) {
    HttpParseResult parsed = parseHttpRequest(input.view());

    if (parsed.status == HttpParseStatus::kIncomplete) {
      if (input.readable() > kMaxRequestBytes) {
        connection.send(HttpResponse::error(413, "request too large").serialize(false));
        return false;
      }
      break;
    }

    if (parsed.status == HttpParseStatus::kError) {
      BOURSE_LOG_DEBUG("http parse error from ", connection.peer(), ": ", parsed.error);
      connection.send(HttpResponse::error(parsed.error_status, parsed.error).serialize(false));
      return false;
    }

    input.retrieve(parsed.consumed);

    HttpResponse response;
    try {
      router_.handle(parsed.request, response);
    } catch (const std::exception& ex) {
      // A handler throwing must become a 500, not a dead connection or a
      // terminate() inside the event loop.
      BOURSE_LOG_ERROR("handler threw for ", parsed.request.path, ": ", ex.what());
      response = HttpResponse::error(500, "internal server error");
    }

    connection.send(response.serialize(parsed.request.keep_alive));
    if (!parsed.request.keep_alive) {
      return false;
    }
  }
  return true;
}

}  // namespace bourse::net
