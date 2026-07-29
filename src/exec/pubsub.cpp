#include "bourse/exec/pubsub.hpp"

#include <algorithm>
#include <utility>

namespace bourse::exec {

void PubSub::setDelivery(DeliveryFn delivery) {
  std::lock_guard<std::mutex> lock(mutex_);
  delivery_ = std::move(delivery);
}

std::size_t PubSub::subscribe(std::uint64_t connection_id, const std::string& channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  channel_to_subscribers_[channel].insert(connection_id);
  auto& channels = subscriber_to_channels_[connection_id];
  channels.insert(channel);
  return channels.size();
}

std::size_t PubSub::unsubscribe(std::uint64_t connection_id, const std::string& channel) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto channel_it = channel_to_subscribers_.find(channel);
  if (channel_it != channel_to_subscribers_.end()) {
    channel_it->second.erase(connection_id);
    if (channel_it->second.empty()) {
      channel_to_subscribers_.erase(channel_it);  // do not leak empty channels
    }
  }

  auto subscriber_it = subscriber_to_channels_.find(connection_id);
  if (subscriber_it == subscriber_to_channels_.end()) {
    return 0;
  }
  subscriber_it->second.erase(channel);
  const std::size_t remaining = subscriber_it->second.size();
  if (remaining == 0) {
    subscriber_to_channels_.erase(subscriber_it);
  }
  return remaining;
}

std::vector<std::string> PubSub::unsubscribeAll(std::uint64_t connection_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> removed;

  auto subscriber_it = subscriber_to_channels_.find(connection_id);
  if (subscriber_it == subscriber_to_channels_.end()) {
    return removed;
  }

  removed.reserve(subscriber_it->second.size());
  for (const std::string& channel : subscriber_it->second) {
    removed.push_back(channel);
    auto channel_it = channel_to_subscribers_.find(channel);
    if (channel_it != channel_to_subscribers_.end()) {
      channel_it->second.erase(connection_id);
      if (channel_it->second.empty()) {
        channel_to_subscribers_.erase(channel_it);
      }
    }
  }
  subscriber_to_channels_.erase(subscriber_it);
  std::sort(removed.begin(), removed.end());
  return removed;
}

void PubSub::removeSubscriber(std::uint64_t connection_id) { (void)unsubscribeAll(connection_id); }

std::size_t PubSub::publish(const std::string& channel, const std::string& payload) {
  std::vector<std::uint64_t> targets;
  DeliveryFn delivery;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channel_to_subscribers_.find(channel);
    if (it == channel_to_subscribers_.end()) {
      return 0;
    }
    targets.assign(it->second.begin(), it->second.end());
    delivery = delivery_;
  }

  // Deliver outside the lock. Holding it across the callback would serialise
  // every publisher behind the slowest subscriber's socket write, and would
  // deadlock outright if a delivery path ever called back into PubSub.
  if (delivery) {
    for (std::uint64_t id : targets) {
      delivery(id, channel, payload);
    }
  }
  return targets.size();
}

std::size_t PubSub::channelCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return channel_to_subscribers_.size();
}

std::size_t PubSub::subscriberCount(const std::string& channel) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = channel_to_subscribers_.find(channel);
  return it == channel_to_subscribers_.end() ? 0 : it->second.size();
}

std::vector<std::string> PubSub::channels() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(channel_to_subscribers_.size());
  for (const auto& [channel, subscribers] : channel_to_subscribers_) {
    out.push_back(channel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t PubSub::subscriptionCount(std::uint64_t connection_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = subscriber_to_channels_.find(connection_id);
  return it == subscriber_to_channels_.end() ? 0 : it->second.size();
}

}  // namespace bourse::exec
