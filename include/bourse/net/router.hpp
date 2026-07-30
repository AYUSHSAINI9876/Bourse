#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/net/http_codec.hpp"

namespace bourse::net {

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

/// One link in the middleware chain.
///
/// `next` is the rest of the chain. Calling it continues; *not* calling it
/// short-circuits, which is how an auth or rate-limit middleware rejects a
/// request without the handler ever seeing it. This is Chain of Responsibility
/// with the continuation passed explicitly rather than held as a `next_`
/// pointer, so the same middleware can be registered on several chains.
using Middleware = std::function<void(const HttpRequest&, HttpResponse&, const std::function<void()>& next)>;

/// Path router with `:name` parameters and a middleware chain.
///
/// Matching is segment-by-segment against a small vector of routes. A radix
/// tree would be asymptotically better, and would matter at hundreds of
/// routes; with a dozen it would be slower in practice and much harder to
/// read, so the linear scan stays until there is a measurement saying
/// otherwise.
class Router {
 public:
  Router& route(std::string method, std::string pattern, HttpHandler handler);

  Router& get(std::string pattern, HttpHandler handler) {
    return route("GET", std::move(pattern), std::move(handler));
  }
  Router& post(std::string pattern, HttpHandler handler) {
    return route("POST", std::move(pattern), std::move(handler));
  }
  Router& put(std::string pattern, HttpHandler handler) {
    return route("PUT", std::move(pattern), std::move(handler));
  }
  Router& del(std::string pattern, HttpHandler handler) {
    return route("DELETE", std::move(pattern), std::move(handler));
  }

  /// Middleware runs in registration order, outermost first.
  Router& use(Middleware middleware);

  /// Resolves and runs the chain. Fills a 404 or 405 when nothing matches --
  /// distinguishing the two matters, because 405 tells a client the path is
  /// real and only the verb is wrong.
  void handle(HttpRequest& request, HttpResponse& response) const;

  [[nodiscard]] std::size_t routeCount() const noexcept { return routes_.size(); }
  [[nodiscard]] std::size_t middlewareCount() const noexcept { return middleware_.size(); }

  /// Splits a path into non-empty segments. Exposed for testing.
  [[nodiscard]] static std::vector<std::string_view> splitPath(std::string_view path);

 private:
  struct Route {
    std::string method;
    std::string pattern;
    std::vector<std::string> segments;
    /// True when the last segment is `*`, which matches the remaining path.
    bool wildcard_tail = false;
    HttpHandler handler;
  };

  [[nodiscard]] const Route* match(const std::string& method, const std::string& path,
                                   std::map<std::string, std::string>& params,
                                   bool& path_exists_other_method) const;

  std::vector<Route> routes_;
  std::vector<Middleware> middleware_;
};

/// Logs method, path, status and duration for every request.
[[nodiscard]] Middleware makeAccessLogMiddleware();
/// Adds permissive CORS headers and answers pre-flight OPTIONS directly.
[[nodiscard]] Middleware makeCorsMiddleware();
/// Counts requests and records latency into the metrics registry.
[[nodiscard]] Middleware makeMetricsMiddleware();

}  // namespace bourse::net
