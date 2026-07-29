#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bourse/exec/reply.hpp"
#include "bourse/net/resp_codec.hpp"

using namespace bourse;
using namespace bourse::net;
using bourse::exec::Reply;

namespace {

std::string multibulk(const std::vector<std::string>& argv) {
  std::string out = "*" + std::to_string(argv.size()) + "\r\n";
  for (const std::string& arg : argv) {
    out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(RespParser, ParsesMultibulkCommand) {
  const std::string wire = multibulk({"SET", "key", "value"});
  const RespCommand parsed = parseRespCommand(wire);

  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  ASSERT_EQ(parsed.argv.size(), 3u);
  EXPECT_EQ(parsed.argv[0], "SET");
  EXPECT_EQ(parsed.argv[1], "key");
  EXPECT_EQ(parsed.argv[2], "value");
  EXPECT_EQ(parsed.consumed, wire.size());
}

TEST(RespParser, ParsesInlineCommandWithCrlf) {
  const RespCommand parsed = parseRespCommand("PING\r\n");
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  ASSERT_EQ(parsed.argv.size(), 1u);
  EXPECT_EQ(parsed.argv[0], "PING");
  EXPECT_EQ(parsed.consumed, 6u);
}

TEST(RespParser, ParsesInlineCommandWithBareLf) {
  // redis-cli --pipe, shell heredocs and netcat all send LF only. Requiring
  // CRLF here is a real interoperability bug, so it gets a dedicated test.
  const RespCommand parsed = parseRespCommand("SET foo bar\n");
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  ASSERT_EQ(parsed.argv.size(), 3u);
  EXPECT_EQ(parsed.argv[2], "bar");
  EXPECT_EQ(parsed.consumed, 12u);
}

TEST(RespParser, InlineCommandHonoursQuotes) {
  const RespCommand parsed = parseRespCommand("SET greeting \"hello world\"\r\n");
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  ASSERT_EQ(parsed.argv.size(), 3u);
  EXPECT_EQ(parsed.argv[2], "hello world");
}

TEST(RespParser, InlineCommandRejectsUnbalancedQuotes) {
  const RespCommand parsed = parseRespCommand("SET k \"unterminated\r\n");
  EXPECT_EQ(parsed.status, RespParseStatus::kProtocolError);
}

TEST(RespParser, ReportsIncompleteForEveryProperPrefix) {
  // The classic TCP bug is assuming one read() delivers one request. Feed the
  // parser every prefix of a valid command and require that it never claims
  // success and never consumes bytes until the frame is whole.
  const std::string wire = multibulk({"HSET", "user", "name", "ayush"});

  for (std::size_t length = 0; length < wire.size(); ++length) {
    const RespCommand parsed = parseRespCommand(std::string_view(wire).substr(0, length));
    EXPECT_EQ(parsed.status, RespParseStatus::kIncomplete)
        << "prefix of length " << length << " was not reported incomplete";
    EXPECT_EQ(parsed.consumed, 0u) << "prefix of length " << length << " consumed bytes";
  }

  const RespCommand complete = parseRespCommand(wire);
  EXPECT_EQ(complete.status, RespParseStatus::kSuccess);
  EXPECT_EQ(complete.consumed, wire.size());
}

TEST(RespParser, ConsumesOnlyOneCommandFromAPipelinedStream) {
  const std::string first = multibulk({"GET", "a"});
  const std::string second = multibulk({"GET", "b"});
  const std::string stream = first + second;

  const RespCommand parsed = parseRespCommand(stream);
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  EXPECT_EQ(parsed.argv[1], "a");
  EXPECT_EQ(parsed.consumed, first.size());

  const RespCommand next = parseRespCommand(std::string_view(stream).substr(parsed.consumed));
  ASSERT_EQ(next.status, RespParseStatus::kSuccess);
  EXPECT_EQ(next.argv[1], "b");
}

TEST(RespParser, HandlesEmptyBulkStrings) {
  const RespCommand parsed = parseRespCommand(multibulk({"SET", "k", ""}));
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  ASSERT_EQ(parsed.argv.size(), 3u);
  EXPECT_TRUE(parsed.argv[2].empty());
}

TEST(RespParser, HandlesBinarySafePayloads) {
  std::string payload("a\0b\r\nc", 6);
  const RespCommand parsed = parseRespCommand(multibulk({"SET", "k", payload}));
  ASSERT_EQ(parsed.status, RespParseStatus::kSuccess);
  EXPECT_EQ(parsed.argv[2], payload);
  EXPECT_EQ(parsed.argv[2].size(), 6u);
}

TEST(RespParser, EmptyArrayIsAWellFormedNoOp) {
  const RespCommand parsed = parseRespCommand("*0\r\n");
  EXPECT_EQ(parsed.status, RespParseStatus::kSuccess);
  EXPECT_TRUE(parsed.argv.empty());
  EXPECT_EQ(parsed.consumed, 4u);
}

TEST(RespParser, RejectsMalformedLengthPrefixes) {
  EXPECT_EQ(parseRespCommand("*abc\r\n").status, RespParseStatus::kProtocolError);
  EXPECT_EQ(parseRespCommand("*2\r\n$xx\r\n").status, RespParseStatus::kProtocolError);
  EXPECT_EQ(parseRespCommand("*1\r\n+OK\r\n").status, RespParseStatus::kProtocolError);
}

TEST(RespParser, RejectsAbsurdLengths) {
  EXPECT_EQ(parseRespCommand("*99999999999\r\n").status, RespParseStatus::kProtocolError);
}

TEST(InlineSplitter, SplitsOnWhitespaceAndQuotes) {
  bool ok = false;
  const std::vector<std::string> argv = splitInlineCommand("  SET   'a b'  \"c d\"  e ", &ok);
  ASSERT_TRUE(ok);
  ASSERT_EQ(argv.size(), 4u);
  EXPECT_EQ(argv[0], "SET");
  EXPECT_EQ(argv[1], "a b");
  EXPECT_EQ(argv[2], "c d");
  EXPECT_EQ(argv[3], "e");
}

// ---------------------------------------------------------------------------
// Reply encoding
// ---------------------------------------------------------------------------

TEST(Reply, EncodesEveryRespType) {
  EXPECT_EQ(Reply::ok().toResp(), "+OK\r\n");
  EXPECT_EQ(Reply::error("ERR nope").toResp(), "-ERR nope\r\n");
  EXPECT_EQ(Reply::integer(42).toResp(), ":42\r\n");
  EXPECT_EQ(Reply::integer(-1).toResp(), ":-1\r\n");
  EXPECT_EQ(Reply::bulkString("foo").toResp(), "$3\r\nfoo\r\n");
  EXPECT_EQ(Reply::bulkString("").toResp(), "$0\r\n\r\n");
  EXPECT_EQ(Reply::null().toResp(), "$-1\r\n");
  EXPECT_EQ(Reply::nullArray().toResp(), "*-1\r\n");
}

TEST(Reply, EncodesNestedArrays) {
  const Reply reply = Reply::array({Reply::bulkString("a"), Reply::integer(2),
                                    Reply::array({Reply::bulkString("nested")})});
  EXPECT_EQ(reply.toResp(), "*3\r\n$1\r\na\r\n:2\r\n*1\r\n$6\r\nnested\r\n");
}

TEST(Reply, BulkStringsAreBinarySafe) {
  const std::string payload("x\0y", 3);
  const Reply reply = Reply::bulkString(payload);
  const std::string encoded = reply.toResp();
  EXPECT_EQ(encoded.size(), 4 + 3 + 2);
  EXPECT_EQ(encoded.substr(0, 4), "$3\r\n");
}

TEST(Reply, RendersJsonForTheRestLayer) {
  EXPECT_EQ(Reply::bulkString("hi").toJson(), "\"hi\"");
  EXPECT_EQ(Reply::integer(7).toJson(), "7");
  EXPECT_EQ(Reply::null().toJson(), "null");
  EXPECT_EQ(Reply::stringArray({"a", "b"}).toJson(), "[\"a\",\"b\"]");
  EXPECT_EQ(Reply::error("boom").toJson(), "{\"error\":\"boom\"}");
}

TEST(Reply, JsonEscapesControlCharacters) {
  const std::string payload = "line\nbreak\ttab\"quote\\slash";
  const std::string json = Reply::bulkString(payload).toJson();
  EXPECT_NE(json.find("\\n"), std::string::npos);
  EXPECT_NE(json.find("\\t"), std::string::npos);
  EXPECT_NE(json.find("\\\""), std::string::npos);
  EXPECT_NE(json.find("\\\\"), std::string::npos);

  const std::string control(1, '\x01');
  EXPECT_NE(Reply::bulkString(control).toJson().find("\\u0001"), std::string::npos);
}
