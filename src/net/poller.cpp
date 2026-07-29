#include "bourse/net/poller.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unordered_map>

#include "bourse/core/logger.hpp"

#if defined(BOURSE_HAVE_EPOLL)
#include <sys/epoll.h>
#include <unistd.h>
#endif

#if defined(BOURSE_PLATFORM_WINDOWS)
#include <winsock2.h>
#define BOURSE_POLL ::WSAPoll
using PollFd = WSAPOLLFD;
#else
#include <poll.h>
#define BOURSE_POLL ::poll
using PollFd = struct pollfd;
#endif

namespace bourse::net {
namespace {

// ---------------------------------------------------------------------------
// poll(2) based fallback
// ---------------------------------------------------------------------------

class PollPoller final : public Poller {
 public:
  Status add(int fd, std::uint32_t interest, std::uint64_t token) override {
    if (index_.find(fd) != index_.end()) {
      return Status::alreadyExists("fd already registered with the poller");
    }
    index_.emplace(fd, entries_.size());
    entries_.push_back(Entry{fd, interest, token});
    dirty_ = true;
    return Status::success();
  }

  Status modify(int fd, std::uint32_t interest, std::uint64_t token) override {
    auto it = index_.find(fd);
    if (it == index_.end()) {
      return Status::notFound("fd not registered with the poller");
    }
    entries_[it->second].interest = interest;
    entries_[it->second].token = token;
    dirty_ = true;
    return Status::success();
  }

  Status remove(int fd) override {
    auto it = index_.find(fd);
    if (it == index_.end()) {
      return Status::notFound("fd not registered with the poller");
    }
    // Swap-and-pop keeps removal O(1); the moved element's index is repaired.
    const std::size_t slot = it->second;
    const std::size_t last = entries_.size() - 1;
    if (slot != last) {
      entries_[slot] = entries_[last];
      index_[entries_[slot].fd] = slot;
    }
    entries_.pop_back();
    index_.erase(it);
    dirty_ = true;
    return Status::success();
  }

  int wait(std::vector<PollEvent>& out, int timeout_ms) override {
    out.clear();
    if (entries_.empty()) {
      return 0;
    }
    rebuildIfNeeded();

    const int ready = BOURSE_POLL(pollfds_.data(), static_cast<unsigned int>(pollfds_.size()), timeout_ms);
    if (ready <= 0) {
      return ready;
    }

    out.reserve(static_cast<std::size_t>(ready));
    for (std::size_t i = 0; i < pollfds_.size() && out.size() < static_cast<std::size_t>(ready); ++i) {
      const short revents = pollfds_[i].revents;
      if (revents == 0) {
        continue;
      }
      PollEvent event;
      event.fd = entries_[i].fd;
      event.token = entries_[i].token;
      if ((revents & POLLIN) != 0) event.flags |= kReadable;
      if ((revents & POLLOUT) != 0) event.flags |= kWritable;
      if ((revents & POLLERR) != 0) event.flags |= kError;
      if ((revents & POLLHUP) != 0) event.flags |= kHangup;
      out.push_back(event);
    }
    return static_cast<int>(out.size());
  }

  [[nodiscard]] std::string_view name() const noexcept override { return "poll"; }
  [[nodiscard]] std::size_t registeredCount() const noexcept override { return entries_.size(); }

 private:
  struct Entry {
    int fd;
    std::uint32_t interest;
    std::uint64_t token;
  };

  void rebuildIfNeeded() {
    if (!dirty_) {
      return;
    }
    pollfds_.clear();
    pollfds_.reserve(entries_.size());
    for (const Entry& entry : entries_) {
      PollFd pfd{};
      pfd.fd = entry.fd;
      pfd.events = 0;
      if ((entry.interest & kReadable) != 0) pfd.events |= POLLIN;
      if ((entry.interest & kWritable) != 0) pfd.events |= POLLOUT;
      pollfds_.push_back(pfd);
    }
    dirty_ = false;
  }

  std::vector<Entry> entries_;
  std::vector<PollFd> pollfds_;
  std::unordered_map<int, std::size_t> index_;
  bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// epoll(7)
// ---------------------------------------------------------------------------

#if defined(BOURSE_HAVE_EPOLL)

class EpollPoller final : public Poller {
 public:
  EpollPoller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (epoll_fd_ < 0) {
      BOURSE_LOG_ERROR("epoll_create1 failed: ", std::strerror(errno));
    }
    ready_.resize(kInitialEventCapacity);
  }

  ~EpollPoller() override {
    if (epoll_fd_ >= 0) {
      ::close(epoll_fd_);
    }
  }

  Status add(int fd, std::uint32_t interest, std::uint64_t token) override {
    return control(EPOLL_CTL_ADD, fd, interest, token);
  }

  Status modify(int fd, std::uint32_t interest, std::uint64_t token) override {
    return control(EPOLL_CTL_MOD, fd, interest, token);
  }

  Status remove(int fd) override {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
      return Status::ioError(std::string("epoll_ctl(DEL): ") + std::strerror(errno));
    }
    if (registered_ > 0) {
      --registered_;
    }
    return Status::success();
  }

  int wait(std::vector<PollEvent>& out, int timeout_ms) override {
    out.clear();
    const int ready = ::epoll_wait(epoll_fd_, ready_.data(), static_cast<int>(ready_.size()), timeout_ms);
    if (ready <= 0) {
      return ready;
    }

    out.reserve(static_cast<std::size_t>(ready));
    for (int i = 0; i < ready; ++i) {
      PollEvent event;
      event.token = ready_[static_cast<std::size_t>(i)].data.u64;
      event.fd = -1;  // epoll carries the token, not the fd; the loop resolves it
      const std::uint32_t revents = ready_[static_cast<std::size_t>(i)].events;
      if ((revents & EPOLLIN) != 0) event.flags |= kReadable;
      if ((revents & EPOLLOUT) != 0) event.flags |= kWritable;
      if ((revents & EPOLLERR) != 0) event.flags |= kError;
      if ((revents & (EPOLLHUP | EPOLLRDHUP)) != 0) event.flags |= kHangup;
      out.push_back(event);
    }

    // Grow the harvest buffer when it fills: a full buffer means events were
    // left for the next iteration, adding a wakeup of latency for those
    // connections. Doubling converges after a couple of busy iterations.
    if (static_cast<std::size_t>(ready) == ready_.size() && ready_.size() < kMaxEventCapacity) {
      ready_.resize(ready_.size() * 2);
    }
    return ready;
  }

  [[nodiscard]] std::string_view name() const noexcept override { return "epoll"; }
  [[nodiscard]] std::size_t registeredCount() const noexcept override { return registered_; }

 private:
  static constexpr std::size_t kInitialEventCapacity = 64;
  static constexpr std::size_t kMaxEventCapacity = 8192;

  Status control(int operation, int fd, std::uint32_t interest, std::uint64_t token) {
    epoll_event event{};
    event.data.u64 = token;
    event.events = 0;
    if ((interest & kReadable) != 0) event.events |= EPOLLIN;
    if ((interest & kWritable) != 0) event.events |= EPOLLOUT;
    // EPOLLRDHUP surfaces a half-close as a readiness event instead of leaving
    // the connection parked until the next read returns 0.
    event.events |= EPOLLRDHUP;

    if (::epoll_ctl(epoll_fd_, operation, fd, &event) != 0) {
      return Status::ioError(std::string("epoll_ctl: ") + std::strerror(errno));
    }
    if (operation == EPOLL_CTL_ADD) {
      ++registered_;
    }
    return Status::success();
  }

  int epoll_fd_ = -1;
  std::vector<epoll_event> ready_;
  std::size_t registered_ = 0;
};

#endif  // BOURSE_HAVE_EPOLL

}  // namespace

std::unique_ptr<Poller> Poller::create() {
#if defined(BOURSE_HAVE_EPOLL)
  return std::make_unique<EpollPoller>();
#else
  return std::make_unique<PollPoller>();
#endif
}

std::unique_ptr<Poller> Poller::createPortable() { return std::make_unique<PollPoller>(); }

}  // namespace bourse::net
