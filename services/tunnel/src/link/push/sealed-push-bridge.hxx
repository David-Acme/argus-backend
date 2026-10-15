#pragma once

#include <link/client/link-client.hxx>
#include <net/poll-loop.hxx>
#include <nats/nats-bus.hxx>
#include <push/push-envelope.hxx>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tunnel::link
{
struct DurableDelivery
{
  PushSealedEnvelope envelope;
  int64_t expiresAtMs{0};
};

namespace sealed_push_json
{
std::optional<DurableDelivery> deliveryFromJson(const std::string& text);
std::string resultToJson(const PushResultEnvelope& result, std::string_view token);
}

class SealedPushLedger
{
public:
  struct Pending
  {
    std::string token;
    NatsBus::DurableSettlement settlement;
  };

  explicit SealedPushLedger(size_t capacity);

  [[nodiscard]] bool contains(const std::string& deliveryId) const;
  void remember(const std::string& deliveryId, Pending pending);
  void replaceSettlement(const std::string& deliveryId, NatsBus::DurableSettlement settlement);
  std::optional<Pending> take(const std::string& deliveryId);

private:
  size_t capacity_;
  std::deque<std::string> order_;
  std::unordered_map<std::string, Pending> pending_;
};

class DeliveredRecently
{
public:
  explicit DeliveredRecently(size_t capacity);

  [[nodiscard]] bool contains(const std::string& deliveryId) const;
  void remember(const std::string& deliveryId);

private:
  size_t capacity_;
  std::deque<std::string> order_;
  std::unordered_set<std::string> seen_;
};

enum class DispatchOutcome : uint8_t
{
  Sent = 0,
  Expired,
  Duplicate,
  LinkDown
};

class SealedPushDispatcher
{
public:
  struct Ports
  {
    std::function<bool(const PushSealedEnvelope&)> send;
    std::function<bool(const PushResultEnvelope&, std::string_view)> publishResult;
    std::function<int64_t()> nowMs;
  };

  SealedPushDispatcher(Ports ports, size_t capacity);

  DispatchOutcome dispatch(const DurableDelivery& delivery, NatsBus::DurableSettlement settlement);
  void result(const PushResultEnvelope& outcome);
  [[nodiscard]] uint64_t expired() const { return expired_; }

private:
  Ports ports_;
  SealedPushLedger pending_;
  DeliveredRecently delivered_;
  uint64_t expired_{0};
  std::unordered_map<std::string, uint32_t> attempts_;
};

enum class ConsumerEdge : uint8_t
{
  None = 0,
  Ready,
  Lost
};

class DurableConsumerState
{
public:
  [[nodiscard]] ConsumerEdge observe(bool attached);

private:
  bool ready_{false};
};

std::string consumerReadyLine(std::string_view subject, std::string_view consumer);
std::string consumerLostLine(std::string_view subject, std::string_view consumer);

class SealedPushBridge
{
public:
  static constexpr size_t kLedgerCapacity = 1024;
  static constexpr int kMaxDeliver = 5;

  SealedPushBridge(PollLoop& loop, std::shared_ptr<NatsBus> bus);

  void attach(LinkClient& client);
  void tick();
  void unsubscribe();

  void onPushResult(const PushResultEnvelope& result);

private:
  void onDelivery(const NatsBus::DurableMessage& message, NatsBus::DurableSettlement settlement);
  [[nodiscard]] NatsBus::DurableInput consumerInput();

  PollLoop& loop_;
  std::shared_ptr<NatsBus> bus_;
  LinkClient* client_{nullptr};
  std::unique_ptr<SealedPushDispatcher> dispatcher_;
  std::optional<uint64_t> consumer_;
  DurableConsumerState consumerState_;
};
}
