#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/exec/command.hpp"
#include "bourse/net/connection.hpp"

namespace bourse::net {

enum class RespParseStatus : std::uint8_t {
  kIncomplete,     ///< need more bytes; leave the buffer untouched
  kSuccess,        ///< argv is a complete command
  kProtocolError,  ///< unrecoverable; the connection must be closed
};

struct RespCommand {
  RespParseStatus status = RespParseStatus::kIncomplete;
  std::vector<std::string> argv;
  std::string error;
  std::size_t consumed = 0;
};

/// Parses one RESP2 command from the front of `input`.
///
/// A free function over a `string_view` rather than a method on the codec, for
/// two reasons: it is trivially fuzzable, and the test suite can feed it a
/// buffer one byte at a time to prove that partial frames are handled. That
/// byte-at-a-time test is the one that catches the classic "assumed the whole
/// request arrives in one read" bug.
///
/// Supports both the multi-bulk form clients actually send (`*3\r\n$3\r\nSET...`)
/// and the inline form a human types into netcat (`SET foo bar\r\n`).
[[nodiscard]] RespCommand parseRespCommand(std::string_view input);

/// Splits an inline command line, honouring single and double quotes so that
/// `SET greeting "hello world"` behaves.
[[nodiscard]] std::vector<std::string> splitInlineCommand(std::string_view line, bool* ok);

/// RESP2 server codec. Wire-compatible with redis-cli and with every standard
/// Redis client library.
class RespCodec final : public ProtocolCodec {
 public:
  RespCodec(const exec::CommandRegistry& registry, exec::ServerContext& server)
      : registry_(registry), server_(server) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "resp"; }

  bool onData(Connection& connection, ByteBuffer& input) override;
  void onClose(Connection& connection) override;

  static constexpr std::size_t kMaxInlineLength = 64 * 1024;
  static constexpr std::int64_t kMaxArrayLength = 1024 * 1024;
  static constexpr std::int64_t kMaxBulkLength = 512 * 1024 * 1024;

 private:
  const exec::CommandRegistry& registry_;
  exec::ServerContext& server_;
};

}  // namespace bourse::net
