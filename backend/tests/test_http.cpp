#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bourse/core/metrics.hpp"
#include "bourse/net/http_codec.hpp"
#include "bourse/net/router.hpp"

using namespace bourse;
using namespace bourse::net;

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(HttpParser, ParsesAMinimalRequest) {
  const std::string wire = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  const HttpParseResult parsed = parseHttpRequest(wire);

  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_EQ(parsed.request.method, "GET");
  EXPECT_EQ(parsed.request.path, "/health");
  EXPECT_EQ(parsed.request.version, "HTTP/1.1");
  EXPECT_EQ(parsed.request.header("Host"), "localhost");
  EXPECT_TRUE(parsed.request.keep_alive) << "HTTP/1.1 defaults to keep-alive";
  EXPECT_EQ(parsed.consumed, wire.size());
}

TEST(HttpParser, HeaderLookupIsCaseInsensitive) {
  const HttpParseResult parsed = parseHttpRequest("GET / HTTP/1.1\r\ncontent-type: application/json\r\n\r\n");
  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_EQ(parsed.request.header("Content-Type"), "application/json");
  EXPECT_EQ(parsed.request.header("CONTENT-TYPE"), "application/json");
}

TEST(HttpParser, ReadsBodiesByContentLength) {
  const std::string wire = "POST /api/command HTTP/1.1\r\nContent-Length: 11\r\n\r\nSET foo bar";
  const HttpParseResult parsed = parseHttpRequest(wire);
  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_EQ(parsed.request.body, "SET foo bar");
  EXPECT_EQ(parsed.consumed, wire.size());
}

TEST(HttpParser, ReportsIncompleteForEveryProperPrefix) {
  // Same discipline as the RESP parser: a request delivered in pieces must
  // never be reported complete early, and must consume nothing until it is.
  const std::string wire = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  for (std::size_t length = 0; length < wire.size(); ++length) {
    const HttpParseResult parsed = parseHttpRequest(std::string_view(wire).substr(0, length));
    EXPECT_EQ(parsed.status, HttpParseStatus::kIncomplete) << "prefix length " << length;
    EXPECT_EQ(parsed.consumed, 0u) << "prefix length " << length;
  }
  EXPECT_EQ(parseHttpRequest(wire).status, HttpParseStatus::kSuccess);
}

TEST(HttpParser, ConsumesOnlyOneRequestFromAPipelinedStream) {
  const std::string first = "GET /a HTTP/1.1\r\n\r\n";
  const std::string second = "GET /b HTTP/1.1\r\n\r\n";
  const HttpParseResult parsed = parseHttpRequest(first + second);
  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_EQ(parsed.request.path, "/a");
  EXPECT_EQ(parsed.consumed, first.size());
}

TEST(HttpParser, SplitsAndDecodesTheQueryString) {
  const HttpParseResult parsed =
      parseHttpRequest("GET /api/orders?symbol=AAPL&side=BUY&note=hello%20world HTTP/1.1\r\n\r\n");
  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_EQ(parsed.request.path, "/api/orders");
  EXPECT_EQ(parsed.request.queryParam("symbol"), "AAPL");
  EXPECT_EQ(parsed.request.queryParam("side"), "BUY");
  EXPECT_EQ(parsed.request.queryParam("note"), "hello world");
  EXPECT_EQ(parsed.request.queryParam("absent", "fallback"), "fallback");
}

TEST(HttpParser, PercentDecodesPathsAndKeepsEncodedSeparatorsInValues) {
  EXPECT_EQ(percentDecode("a%2Fb"), "a/b");
  EXPECT_EQ(percentDecode("plus+sign"), "plus sign");
  EXPECT_EQ(percentDecode("%ZZbad"), "%ZZbad") << "malformed escapes pass through unchanged";

  // The query is split on & and = *before* decoding, so an encoded ampersand
  // inside a value is not mistaken for a separator.
  const auto query = parseQueryString("a=1%262&b=3");
  EXPECT_EQ(query.at("a"), "1&2");
  EXPECT_EQ(query.at("b"), "3");
}

TEST(HttpParser, HonoursConnectionClose) {
  const HttpParseResult closed = parseHttpRequest("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
  ASSERT_EQ(closed.status, HttpParseStatus::kSuccess);
  EXPECT_FALSE(closed.request.keep_alive);

  const HttpParseResult legacy = parseHttpRequest("GET / HTTP/1.0\r\n\r\n");
  ASSERT_EQ(legacy.status, HttpParseStatus::kSuccess);
  EXPECT_FALSE(legacy.request.keep_alive) << "HTTP/1.0 defaults to close";

  const HttpParseResult legacy_keepalive =
      parseHttpRequest("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
  EXPECT_TRUE(legacy_keepalive.request.keep_alive);
}

TEST(HttpParser, RejectsMalformedAndUnsupportedRequests) {
  EXPECT_EQ(parseHttpRequest("GARBAGE\r\n\r\n").status, HttpParseStatus::kError);
  EXPECT_EQ(parseHttpRequest("GET /\r\n\r\n").status, HttpParseStatus::kError);

  const HttpParseResult old_version = parseHttpRequest("GET / HTTP/0.9\r\n\r\n");
  EXPECT_EQ(old_version.status, HttpParseStatus::kError);
  EXPECT_EQ(old_version.error_status, 505);

  // Chunked is refused rather than silently ignored -- ignoring
  // Transfer-Encoding is a request-smuggling bug.
  const HttpParseResult chunked =
      parseHttpRequest("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  EXPECT_EQ(chunked.status, HttpParseStatus::kError);
  EXPECT_EQ(chunked.error_status, 411);

  const HttpParseResult bad_length = parseHttpRequest("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
  EXPECT_EQ(bad_length.status, HttpParseStatus::kError);
}

TEST(HttpParser, DetectsWebSocketUpgradeIntent) {
  const HttpParseResult parsed =
      parseHttpRequest("GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
  ASSERT_EQ(parsed.status, HttpParseStatus::kSuccess);
  EXPECT_TRUE(parsed.request.wantsWebSocketUpgrade());
  EXPECT_FALSE(parseHttpRequest("GET / HTTP/1.1\r\n\r\n").request.wantsWebSocketUpgrade());
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

TEST(HttpResponseTest, AlwaysEmitsContentLength) {
  // Without it the client cannot find the end of the body without a close,
  // which silently defeats keep-alive.
  const std::string wire = HttpResponse::json("{}").serialize(true);
  EXPECT_NE(wire.find("Content-Length: 2"), std::string::npos);
  EXPECT_NE(wire.find("Connection: keep-alive"), std::string::npos);
  EXPECT_NE(wire.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(wire.find("\r\n\r\n{}"), std::string::npos);
}

TEST(HttpResponseTest, ReflectsTheKeepAliveDecision) {
  EXPECT_NE(HttpResponse::text("x").serialize(false).find("Connection: close"), std::string::npos);
}

TEST(HttpResponseTest, ErrorBodiesAreValidJson) {
  const HttpResponse response = HttpResponse::error(404, "no route for /a\"b");
  EXPECT_EQ(response.status, 404);
  EXPECT_NE(response.body.find("\\\""), std::string::npos) << "quotes must be escaped";
  EXPECT_NE(response.body.find("\"status\":404"), std::string::npos);
}

TEST(HttpResponseTest, KnowsItsReasonPhrases) {
  EXPECT_STREQ(HttpResponse::reasonPhrase(200), "OK");
  EXPECT_STREQ(HttpResponse::reasonPhrase(404), "Not Found");
  EXPECT_STREQ(HttpResponse::reasonPhrase(405), "Method Not Allowed");
  EXPECT_STREQ(HttpResponse::reasonPhrase(503), "Service Unavailable");
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

namespace {

HttpRequest makeRequest(std::string method, std::string path) {
  HttpRequest request;
  request.method = std::move(method);
  request.path = std::move(path);
  request.version = "HTTP/1.1";
  return request;
}

}  // namespace

TEST(RouterTest, SplitsPathsIntoSegments) {
  EXPECT_TRUE(Router::splitPath("/").empty());
  EXPECT_EQ(Router::splitPath("/a/b/c").size(), 3u);
  EXPECT_EQ(Router::splitPath("///a//b//").size(), 2u) << "empty segments must collapse";
}

TEST(RouterTest, DispatchesStaticRoutes) {
  Router router;
  bool hit = false;
  router.get("/health", [&](const HttpRequest&, HttpResponse& response) {
    hit = true;
    response = HttpResponse::text("ok");
  });

  HttpRequest request = makeRequest("GET", "/health");
  HttpResponse response;
  router.handle(request, response);

  EXPECT_TRUE(hit);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "ok");
}

TEST(RouterTest, ExtractsPathParameters) {
  Router router;
  std::string captured_symbol;
  std::string captured_id;
  router.del("/api/orders/:symbol/:id", [&](const HttpRequest& request, HttpResponse& response) {
    captured_symbol = request.pathParam("symbol");
    captured_id = request.pathParam("id");
    response = HttpResponse::noContent();
  });

  HttpRequest request = makeRequest("DELETE", "/api/orders/AAPL/42");
  HttpResponse response;
  router.handle(request, response);

  EXPECT_EQ(captured_symbol, "AAPL");
  EXPECT_EQ(captured_id, "42");
  EXPECT_EQ(response.status, 204);
}

TEST(RouterTest, DistinguishesMissingPathFromWrongMethod) {
  Router router;
  router.get("/thing",
             [](const HttpRequest&, HttpResponse& response) { response = HttpResponse::text("x"); });

  HttpRequest missing = makeRequest("GET", "/nothing");
  HttpResponse response_missing;
  router.handle(missing, response_missing);
  EXPECT_EQ(response_missing.status, 404);

  // 405 tells the client the URL is right and only the verb is wrong; the
  // distinction is genuinely useful, so it is worth a test.
  HttpRequest wrong_verb = makeRequest("POST", "/thing");
  HttpResponse response_wrong;
  router.handle(wrong_verb, response_wrong);
  EXPECT_EQ(response_wrong.status, 405);
}

TEST(RouterTest, DoesNotMatchOnSegmentCount) {
  Router router;
  router.get("/a/:id",
             [](const HttpRequest&, HttpResponse& response) { response = HttpResponse::text("x"); });

  HttpRequest too_long = makeRequest("GET", "/a/b/c");
  HttpResponse response;
  router.handle(too_long, response);
  EXPECT_EQ(response.status, 404);
}

TEST(RouterTest, RunsMiddlewareOutermostFirst) {
  Router router;
  std::vector<std::string> order;

  router.use([&](const HttpRequest&, HttpResponse&, const std::function<void()>& next) {
    order.emplace_back("first-before");
    next();
    order.emplace_back("first-after");
  });
  router.use([&](const HttpRequest&, HttpResponse&, const std::function<void()>& next) {
    order.emplace_back("second-before");
    next();
    order.emplace_back("second-after");
  });
  router.get("/x", [&](const HttpRequest&, HttpResponse& response) {
    order.emplace_back("handler");
    response = HttpResponse::text("done");
  });

  HttpRequest request = makeRequest("GET", "/x");
  HttpResponse response;
  router.handle(request, response);

  EXPECT_EQ(order, (std::vector<std::string>{"first-before", "second-before", "handler", "second-after",
                                             "first-after"}));
}

TEST(RouterTest, MiddlewareCanShortCircuit) {
  // Not calling next() must stop the chain -- this is how auth or rate limiting
  // rejects a request without the handler ever running.
  Router router;
  bool handler_ran = false;

  router.use([](const HttpRequest&, HttpResponse& response, const std::function<void()>&) {
    response = HttpResponse::error(401, "unauthorized");
  });
  router.get("/secret", [&](const HttpRequest&, HttpResponse& response) {
    handler_ran = true;
    response = HttpResponse::text("secrets");
  });

  HttpRequest request = makeRequest("GET", "/secret");
  HttpResponse response;
  router.handle(request, response);

  EXPECT_FALSE(handler_ran) << "short-circuiting middleware still reached the handler";
  EXPECT_EQ(response.status, 401);
}

TEST(RouterTest, CorsMiddlewareAnswersPreflightWithoutAHandler) {
  Router router;
  router.use(makeCorsMiddleware());
  bool handler_ran = false;
  router.post("/api/thing", [&](const HttpRequest&, HttpResponse& response) {
    handler_ran = true;
    response = HttpResponse::text("x");
  });

  HttpRequest preflight = makeRequest("OPTIONS", "/api/thing");
  HttpResponse response;
  router.handle(preflight, response);

  EXPECT_EQ(response.status, 204);
  EXPECT_FALSE(handler_ran);
  EXPECT_EQ(response.headers.at("Access-Control-Allow-Origin"), "*");
}

TEST(RouterTest, MetricsMiddlewareRecordsWithoutChangingTheResponse) {
  Router router;
  router.use(makeMetricsMiddleware());
  router.get("/x", [](const HttpRequest&, HttpResponse& response) { response = HttpResponse::text("ok"); });

  const std::uint64_t before = MetricsRegistry::instance().counter("bourse_http_requests_total").value();

  HttpRequest request = makeRequest("GET", "/x");
  HttpResponse response;
  router.handle(request, response);

  EXPECT_EQ(response.body, "ok");
  EXPECT_EQ(MetricsRegistry::instance().counter("bourse_http_requests_total").value(), before + 1);
}
