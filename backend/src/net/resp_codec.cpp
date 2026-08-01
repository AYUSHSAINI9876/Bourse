#include "bourse/net/resp_codec.hpp"

#include <cctype>
#include <charconv>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/exec/pubsub.hpp"

namespace bourse::net {
namespace {

constexpr std::string_view kCrlf = "\r\n";

/// Parses a RESP length prefix. Returns false on anything that is not exactly
/// a base-10 integer -- RESP is strict here, and being lenient is how parsers
/// end up with integer-overflow bugs.
bool parseLengthPrefix(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

std::vector<std::string> splitInlineCommand(std::string_view line, bool* ok) {
  std::vector<std::string> argv;
  std::string current;
  bool in_token = false;
  char quote = '\0';

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];

    if (quote != '\0') {
      if (c == '\\' && i + 1 < line.size()) {
        current.push_back(line[++i]);
      } else if (c == quote) {
        quote = '\0';
      } else {
        current.push_back(c);
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      quote = c;
      in_token = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (in_token) {
        argv.push_back(std::move(current));
        current.clear();
        in_token = false;
      }
      continue;
    }
    current.push_back(c);
    in_token = true;
  }

  if (quote != '\0') {
    if (ok != nullptr) {
      *ok = false;  // unbalanced quotes
    }
    return {};
  }
  if (in_token) {
    argv.push_back(std::move(current));
  }
  if (ok != nullptr) {
    *ok = true;
  }
  return argv;
}

RespCommand parseRespCommand(std::string_view input) {
  RespCommand out;
  if (input.empty()) {
    return out;  // incomplete
  }

  // ---- inline form -------------------------------------------------------
  if (input[0] != '*') {
    // Terminate on a bare '\n' and strip an optional preceding '\r'.
    //
    // Requiring a full CRLF here looks stricter and more correct, and it is
    // wrong: Redis accepts either terminator for inline commands, so anything
    // producing Unix line endings -- `redis-cli --pipe`, a shell heredoc,
    // netcat driven from a text file -- speaks LF only. Demanding CRLF made
    // every such client hang waiting for a reply that never came.
    const std::size_t eol = input.find('\n');
    if (eol == std::string_view::npos) {
      if (input.size() > RespCodec::kMaxInlineLength) {
        out.status = RespParseStatus::kProtocolError;
        out.error = "too big inline request";
      }
      return out;  // incomplete
    }

    std::string_view line = input.substr(0, eol);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    bool ok = false;
    out.argv = splitInlineCommand(line, &ok);
    if (!ok) {
      out.status = RespParseStatus::kProtocolError;
      out.error = "unbalanced quotes in request";
      return out;
    }
    out.consumed = eol + 1;
    out.status = RespParseStatus::kSuccess;
    return out;
  }

  // ---- multi-bulk form ---------------------------------------------------
  std::size_t cursor = 0;
  std::size_t eol = input.find(kCrlf, cursor);
  if (eol == std::string_view::npos) {
    return out;  // incomplete
  }

  std::int64_t argc = 0;
  if (!parseLengthPrefix(input.substr(1, eol - 1), argc)) {
    out.status = RespParseStatus::kProtocolError;
    out.error = "invalid multibulk length";
    return out;
  }
  if (argc > RespCodec::kMaxArrayLength) {
    out.status = RespParseStatus::kProtocolError;
    out.error = "invalid multibulk length";
    return out;
  }
  cursor = eol + kCrlf.size();

  if (argc <= 0) {
    // A null or empty array is a well-formed no-op; consume and move on.
    out.consumed = cursor;
    out.status = RespParseStatus::kSuccess;
    return out;
  }

  out.argv.reserve(static_cast<std::size_t>(argc));
  for (std::int64_t i = 0; i < argc; ++i) {
    if (cursor >= input.size()) {
      return RespCommand{};  // incomplete -- discard the partial argv
    }
    if (input[cursor] != '$') {
      out.status = RespParseStatus::kProtocolError;
      out.error = "expected '$', got '" + std::string(1, input[cursor]) + "'";
      return out;
    }

    eol = input.find(kCrlf, cursor);
    if (eol == std::string_view::npos) {
      return RespCommand{};  // incomplete
    }

    std::int64_t length = 0;
    if (!parseLengthPrefix(input.substr(cursor + 1, eol - cursor - 1), length)) {
      out.status = RespParseStatus::kProtocolError;
      out.error = "invalid bulk length";
      return out;
    }
    if (length > RespCodec::kMaxBulkLength) {
      out.status = RespParseStatus::kProtocolError;
      out.error = "invalid bulk length";
      return out;
    }
    cursor = eol + kCrlf.size();

    if (length < 0) {
      out.argv.emplace_back();  // null bulk inside an array -> empty argument
      continue;
    }

    const auto needed = static_cast<std::size_t>(length) + kCrlf.size();
    if (input.size() - cursor < needed) {
      return RespCommand{};  // incomplete
    }
    out.argv.emplace_back(input.substr(cursor, static_cast<std::size_t>(length)));
    cursor += needed;
  }

  out.consumed = cursor;
  out.status = RespParseStatus::kSuccess;
  return out;
}

bool RespCodec::onData(Connection& connection, ByteBuffer& input) {
  static Histogram& latency = MetricsRegistry::instance().histogram("bourse_command_latency_nanos");
  static Counter& processed = MetricsRegistry::instance().counter("bourse_commands_processed_total");
  static Counter& errors = MetricsRegistry::instance().counter("bourse_command_errors_total");

  std::string outbound;

  for (;;) {
    // `view()` hands the parser a zero-copy window over the unread bytes. The
    // first version of this loop built a std::string per iteration; removing
    // that copy is the single biggest throughput change in the project's
    // history (see docs/benchmarks.md).
    const RespCommand parsed = parseRespCommand(input.view());

    if (parsed.status == RespParseStatus::kIncomplete) {
      break;
    }
    if (parsed.status == RespParseStatus::kProtocolError) {
      BOURSE_LOG_DEBUG("protocol error from ", connection.peer(), ": ", parsed.error);
      outbound.append("-ERR Protocol error: ").append(parsed.error).append("\r\n");
      connection.send(outbound);
      return false;  // unrecoverable: the stream is out of sync
    }

    input.retrieve(parsed.consumed);
    if (parsed.argv.empty()) {
      continue;  // empty inline line or null array
    }

    const std::int64_t started = nowNanos();
    // The principal is copied in from the connection and copied back out
    // afterwards, so AUTH -- which is an ordinary command with no special case
    // in this loop -- can change the identity for every command that follows.
    exec::CommandContext context{server_, &connection, connection.principal(), connection.peer()};
    const exec::Reply reply = registry_.dispatch(context, parsed.argv);
    if (context.principal.username != connection.principal().username ||
        context.principal.role != connection.principal().role) {
      connection.setPrincipal(context.principal);
    }
    latency.record(static_cast<std::uint64_t>(nowNanos() - started));
    processed.increment();
    if (reply.isError()) {
      errors.increment();
    }

    reply.encodeResp(outbound);

    // QUIT is the one verb that closes the connection; it answers +OK first.
    if (parsed.argv[0].size() == 4 && (parsed.argv[0][0] == 'Q' || parsed.argv[0][0] == 'q')) {
      std::string upper;
      for (char c : parsed.argv[0]) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      }
      if (upper == "QUIT") {
        connection.send(outbound);
        return false;
      }
    }
  }

  // One send() for the whole batch. A pipelined client that ships 100 commands
  // in a single packet gets one write(2) back instead of 100.
  if (!outbound.empty()) {
    connection.send(outbound);
  }
  return true;
}

void RespCodec::onClose(Connection& connection) {
  if (server_.pubsub != nullptr) {
    server_.pubsub->removeSubscriber(connection.id());
  }
}

}  // namespace bourse::net
