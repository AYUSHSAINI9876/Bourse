#include "bourse/net/router.hpp"

#include <utility>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"

namespace bourse::net {

std::vector<std::string_view> Router::splitPath(std::string_view path) {
  std::vector<std::string_view> segments;
  std::size_t cursor = 0;
  while (cursor < path.size()) {
    while (cursor < path.size() && path[cursor] == '/') {
      ++cursor;
    }
    if (cursor >= path.size()) {
      break;
    }
    const std::size_t start = cursor;
    while (cursor < path.size() && path[cursor] != '/') {
      ++cursor;
    }
    segments.push_back(path.substr(start, cursor - start));
  }
  return segments;
}

Router& Router::route(std::string method, std::string pattern, HttpHandler handler) {
  Route entry;
  entry.method = std::move(method);
  entry.pattern = std::move(pattern);
  for (std::string_view segment : splitPath(entry.pattern)) {
    entry.segments.emplace_back(segment);
  }
  if (!entry.segments.empty() && entry.segments.back() == "*") {
    entry.wildcard_tail = true;
    entry.segments.pop_back();
  }
  entry.handler = std::move(handler);
  routes_.push_back(std::move(entry));
  return *this;
}

Router& Router::use(Middleware middleware) {
  middleware_.push_back(std::move(middleware));
  return *this;
}

const Router::Route* Router::match(const std::string& method, const std::string& path,
                                   std::map<std::string, std::string>& params,
                                   bool& path_exists_other_method) const {
  const std::vector<std::string_view> segments = splitPath(path);
  path_exists_other_method = false;

  for (const Route& route : routes_) {
    if (route.wildcard_tail) {
      if (segments.size() < route.segments.size()) {
        continue;
      }
    } else if (segments.size() != route.segments.size()) {
      continue;
    }

    std::map<std::string, std::string> candidate_params;
    bool matched = true;
    for (std::size_t i = 0; i < route.segments.size(); ++i) {
      const std::string& pattern = route.segments[i];
      if (!pattern.empty() && pattern.front() == ':') {
        candidate_params.emplace(pattern.substr(1), std::string(segments[i]));
        continue;
      }
      if (pattern != segments[i]) {
        matched = false;
        break;
      }
    }
    if (!matched) {
      continue;
    }

    if (route.method != method) {
      // Remember that the path exists so the caller can answer 405 rather than
      // 404 -- the distinction tells a client the URL is right and only the
      // verb is wrong.
      path_exists_other_method = true;
      continue;
    }

    params = std::move(candidate_params);
    return &route;
  }
  return nullptr;
}

void Router::handle(HttpRequest& request, HttpResponse& response) const {
  bool other_method = false;
  std::map<std::string, std::string> params;
  const Route* route = match(request.method, request.path, params, other_method);
  request.params = std::move(params);

  // The innermost link: run the handler, or produce the right kind of miss.
  std::function<void()> next = [&] {
    if (route != nullptr) {
      route->handler(request, response);
      return;
    }
    if (other_method) {
      response = HttpResponse::error(405, "method not allowed for " + request.path);
      return;
    }
    response = HttpResponse::error(404, "no route for " + request.path);
  };

  // Wrap from the inside out so that the first-registered middleware ends up
  // outermost and therefore runs first.
  for (auto it = middleware_.rbegin(); it != middleware_.rend(); ++it) {
    const Middleware* current = &(*it);
    std::function<void()> inner = std::move(next);
    next = [current, &request, &response, inner = std::move(inner)] {
      (*current)(request, response, inner);
    };
  }

  next();
}

// ---------------------------------------------------------------------------
// Stock middleware
// ---------------------------------------------------------------------------

Middleware makeAccessLogMiddleware() {
  return [](const HttpRequest& request, HttpResponse& response, const std::function<void()>& next) {
    const Stopwatch watch;
    next();
    BOURSE_LOG_INFO(request.method, ' ', request.path, " -> ", response.status, ' ',
                    static_cast<std::int64_t>(watch.elapsedMicros()), "us");
  };
}

Middleware makeCorsMiddleware() {
  return [](const HttpRequest& request, HttpResponse& response, const std::function<void()>& next) {
    if (request.method == "OPTIONS") {
      // Answer pre-flight without ever reaching a handler -- the
      // short-circuiting case that justifies the chain being explicit.
      response.status = 204;
      response.setHeader("Access-Control-Allow-Origin", "*");
      response.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      response.setHeader("Access-Control-Allow-Headers", "Content-Type");
      response.setHeader("Access-Control-Max-Age", "86400");
      return;
    }
    next();
    response.setHeader("Access-Control-Allow-Origin", "*");
  };
}

Middleware makeMetricsMiddleware() {
  return [](const HttpRequest& request, HttpResponse& response, const std::function<void()>& next) {
    static Counter& requests = MetricsRegistry::instance().counter("bourse_http_requests_total");
    static Counter& errors = MetricsRegistry::instance().counter("bourse_http_errors_total");
    static Histogram& latency = MetricsRegistry::instance().histogram("bourse_http_latency_nanos");

    const std::int64_t started = nowNanos();
    next();
    latency.record(static_cast<std::uint64_t>(nowNanos() - started));
    requests.increment();
    if (response.status >= 400) {
      errors.increment();
    }
    (void)request;
  };
}

}  // namespace bourse::net
