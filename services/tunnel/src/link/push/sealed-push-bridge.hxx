#pragma once

#include <link/client/link-client.hxx>
#include <net/poll-loop.hxx>
#include <nats/nats-bus.hxx>
#include <push/push-envelope.hxx>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tunnel::link
{
namespace sealed_push_json
{
std::optional<PushSealedEnvelope> deliveryFromJson(const std::string& text);
std::string resultToJson(const PushResultEnvelope& result, std::string_view token);
}

class SealedPushLedger
{
public:
  explicit SealedPushLedger(size_t capacity);

  void remember(const std::string& deliveryId, const std::string& token);
  std::string take(const std::string& deliveryId);

private:
  size_t capacity_;
  std::deque<std::string> order_;
  std::unordered_map<std::string, std::string> tokens_;
};

class SealedPushBridge
{
public:
  static constexpr size_t kLedgerCapacity = 1024;

  SealedPushBridge(PollLoop& loop, std::shared_ptr<NatsBus> bus);

  void attach(LinkClient& client);
  bool subscribe();
  void unsubscribe();

  void onPushResult(const PushResultEnvelope& result);

private:
  void onDelivery(std::string_view payload);
  void sendOnLoop(const PushSealedEnvelope& envelope);

  PollLoop& loop_;
  std::shared_ptr<NatsBus> bus_;
  LinkClient* client_{nullptr};
  SealedPushLedger ledger_{kLedgerCapacity};
  std::optional<uint64_t> subscription_;
};
}
