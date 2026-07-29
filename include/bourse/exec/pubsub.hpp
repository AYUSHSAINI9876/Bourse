#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bourse::exec {

/// Channel fan-out.
///
/// Subscribers are identified by connection id rather than by pointer. A
/// publish can race with a disconnect, and holding a raw `Connection*` in the
/// subscriber table would mean publishing into freed memory; an id forces the
/// delivery path back through the server's connection table, which is the only
/// place that knows whether the connection still exists.
///
/// Delivery itself is a callback the server installs, so this class has no
/// dependency on the network layer at all and can be unit-tested standalone.
class PubSub {
 public:
  /// Invoked for each matching subscriber. Implementations must be safe to call
  /// from any thread; the server's implementation posts to the owning loop.
  using DeliveryFn = std::function<void(std::uint64_t connection_id, const std::string& channel,
                                        const std::string& payload)>;

  void setDelivery(DeliveryFn delivery);

  /// Returns the subscriber's new total subscription count.
  std::size_t subscribe(std::uint64_t connection_id, const std::string& channel);
  std::size_t unsubscribe(std::uint64_t connection_id, const std::string& channel);
  /// Unsubscribes from everything; returns the channels that were left.
  std::vector<std::string> unsubscribeAll(std::uint64_t connection_id);

  /// Drops every trace of a connection. Called from the codec's onClose.
  void removeSubscriber(std::uint64_t connection_id);

  /// Returns the number of subscribers the message was delivered to.
  std::size_t publish(const std::string& channel, const std::string& payload);

  [[nodiscard]] std::size_t channelCount() const;
  [[nodiscard]] std::size_t subscriberCount(const std::string& channel) const;
  [[nodiscard]] std::vector<std::string> channels() const;
  [[nodiscard]] std::size_t subscriptionCount(std::uint64_t connection_id) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::unordered_set<std::uint64_t>> channel_to_subscribers_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::string>> subscriber_to_channels_;
  DeliveryFn delivery_;
};

}  // namespace bourse::exec
