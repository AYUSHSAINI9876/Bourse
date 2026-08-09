#include "bourse/core/logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "bourse/core/clock.hpp"

namespace bourse {

const char* toString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace: return "TRACE";
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO ";
    case LogLevel::kWarn: return "WARN ";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kFatal: return "FATAL";
    case LogLevel::kOff: return "OFF  ";
  }
  return "?????";
}

LogLevel parseLogLevel(std::string_view text, LogLevel fallback) noexcept {
  std::string lowered;
  lowered.reserve(text.size());
  for (char c : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == "trace")
    return LogLevel::kTrace;
  if (lowered == "debug")
    return LogLevel::kDebug;
  if (lowered == "info")
    return LogLevel::kInfo;
  if (lowered == "warn" || lowered == "warning")
    return LogLevel::kWarn;
  if (lowered == "error")
    return LogLevel::kError;
  if (lowered == "fatal")
    return LogLevel::kFatal;
  if (lowered == "off" || lowered == "none")
    return LogLevel::kOff;
  return fallback;
}

std::string formatTimestamp(std::int64_t epoch_millis) {
  const auto seconds = static_cast<std::time_t>(epoch_millis / 1000);
  const auto millis = static_cast<int>(epoch_millis % 1000);

  std::tm tm_value{};
#if defined(_WIN32)
  localtime_s(&tm_value, &seconds);
#else
  localtime_r(&seconds, &tm_value);
#endif

  std::array<char, 40> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                                    tm_value.tm_year + 1900, tm_value.tm_mon + 1, tm_value.tm_mday,
                                    tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec, millis);
  if (written <= 0) {
    return "0000-00-00 00:00:00.000";
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

namespace detail {

const char* shortFileName(const char* path) noexcept {
  if (path == nullptr) {
    return "";
  }
  const char* best = path;
  int separators_seen = 0;
  // Keep the last two path components: "net/event_loop.cpp".
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      ++separators_seen;
    }
  }
  int wanted = separators_seen - 1;
  if (wanted < 0) {
    return path;
  }
  int seen = 0;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      ++seen;
      if (seen > wanted) {
        best = p + 1;
        break;
      }
    }
  }
  return best;
}

}  // namespace detail

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::Logger() {
  sink_ = [](std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
  };
  if (const char* env = std::getenv("BOURSE_LOG_LEVEL"); env != nullptr) {
    level_.store(parseLogLevel(env, LogLevel::kInfo), std::memory_order_relaxed);
  }
}

Logger::~Logger() {
  setAsync(false);
}

void Logger::setSink(Sink sink) {
  const bool was_async = async_.load(std::memory_order_acquire);
  setAsync(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
  }
  if (was_async) {
    setAsync(true);
  }
}

void Logger::setAsync(bool async) {
  if (async == async_.load(std::memory_order_acquire)) {
    return;
  }

  if (async) {
    running_.store(true, std::memory_order_release);
    async_.store(true, std::memory_order_release);
    consumer_ = std::thread([this] { consumerLoop(); });
    return;
  }

  running_.store(false, std::memory_order_release);
  async_.store(false, std::memory_order_release);
  not_empty_.notify_all();
  if (consumer_.joinable()) {
    consumer_.join();
  }
  // Anything the consumer did not reach is delivered inline so no log line is
  // ever silently dropped on shutdown.
  std::deque<std::string> leftovers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    leftovers.swap(queue_);
    for (const std::string& line : leftovers) {
      deliver(line);
    }
  }
  drained_.notify_all();
}

void Logger::submit(LogLevel level, const char* file, int line, std::string message) {
  std::string formatted = format(level, file, line, message);

  if (!async_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(mutex_);
    deliver(formatted);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(formatted));
  }
  not_empty_.notify_one();
}

void Logger::flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  drained_.wait(lock, [this] { return queue_.empty(); });
}

void Logger::consumerLoop() {
  std::deque<std::string> batch;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock, [this] { return !queue_.empty() || !running_.load(std::memory_order_acquire); });
      if (queue_.empty()) {
        if (!running_.load(std::memory_order_acquire)) {
          return;
        }
        continue;
      }
      // Batch-swap rather than pop one at a time: producers only contend for
      // the lock long enough to push, not for the duration of the I/O.
      batch.swap(queue_);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const std::string& line : batch) {
        deliver(line);
      }
    }
    batch.clear();
    drained_.notify_all();
  }
}

void Logger::deliver(const std::string& line) {
  if (sink_) {
    sink_(line);
  }
}

std::string Logger::format(LogLevel level, const char* file, int line, const std::string& message) const {
  std::string out;
  out.reserve(message.size() + 64);
  out.append(formatTimestamp(nowMillis()));
  out.append(" [");
  out.append(toString(level));
  out.append("] ");
  out.append(message);
  out.append("  (");
  out.append(detail::shortFileName(file));
  out.push_back(':');
  out.append(std::to_string(line));
  out.push_back(')');
  return out;
}

}  // namespace bourse
