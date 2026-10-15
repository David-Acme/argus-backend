#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/repositories/notification/notification-query.hxx>
#include <shared/services/push-delivery/push-alert-writer.hxx>
#include <shared/services/push-delivery/push-delivery-wire.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view kRecipientPublicHex =
    "5ee54ad74d51ba57c198138c2f690ea8efaa105f4f5ea7c518c9a905cbab7048";
constexpr int64_t kNow = 1800000000;
constexpr int64_t kAlertTtlSeconds = 3600;

std::string fromHex(std::string_view hex)
{
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return out;
}

std::string base64UrlOf(const std::string& bytes)
{
  std::string text = base64::encode(bytes);
  for (char& symbol : text) {
    if (symbol == '+')
      symbol = '-';
    else if (symbol == '/')
      symbol = '_';
  }
  while (!text.empty() && text.back() == '=')
    text.pop_back();
  return text;
}

class MixedDevices final : public PushDeviceDirectory
{
public:
  drogon::Task<std::vector<PushDevice>> devicesOf(int64_t userId) const override
  {
    const std::string key = base64UrlOf(fromHex(kRecipientPublicHex));
    if (userId != 42)
      co_return std::vector<PushDevice>{};
    co_return std::vector<PushDevice>{{.provider = PushProvider::Fcm,
                                       .token = "fcm-token-0001",
                                       .publicKey = key,
                                       .keyId = "key-0001"},
                                      {.provider = PushProvider::Apns,
                                       .token = "apns-token-0001",
                                       .publicKey = key,
                                       .keyId = "key-0002"},
                                      {.provider = PushProvider::ApnsVoip,
                                       .token = "voip-token-0001",
                                       .publicKey = key,
                                       .keyId = "key-0003"}};
  }
};

class RecordingOutbox final : public PushDeliveryOutbox
{
public:
  drogon::Task<void> record(PushDeliveryRecord entry) const override
  {
    entries.push_back(std::move(entry));
    co_return;
  }

  mutable std::vector<PushDeliveryRecord> entries;
};

NotificationCreateInput alertFor(int64_t userId)
{
  NotificationCreateInput input;
  input.userId = userId;
  input.type = "camera";
  input.title = "Puerta";
  input.body = "Movimiento en la puerta";
  input.data = Json::Value(Json::objectValue);
  input.data["lang"] = "es";
  return input;
}

PushAlertWriter writerOver(const std::shared_ptr<RecordingOutbox>& outbox)
{
  return PushAlertWriter(PushAlertWriterDependencies{.devices = std::make_shared<MixedDevices>(),
                                                     .outbox = outbox,
                                                     .clock = [] { return kNow; }});
}

Json::Value payloadOf(const PushDeliveryRecord& entry)
{
  return json_util::fromString(entry.payload);
}
}

TEST_CASE("an alert is recorded for each device that can carry one, never for a VoIP token")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  NotificationAlertRound round;
  round.inputs = {alertFor(42)};
  round.notificationIds = {901};

  drogon::sync_wait(writerOver(outbox).write(round));

  REQUIRE(outbox->entries.size() == 2);
  CHECK(payloadOf(outbox->entries[0])["provider"].asString() == "fcm");
  CHECK(payloadOf(outbox->entries[1])["provider"].asString() == "apns");
  for (const auto& entry : outbox->entries)
    CHECK(payloadOf(entry)["kind"].asString() == "alert");
}

TEST_CASE("an alert expires after its own one-hour TTL and carries that TTL outside the sealed bytes")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  NotificationAlertRound round;
  round.inputs = {alertFor(42)};
  round.notificationIds = {901};

  drogon::sync_wait(writerOver(outbox).write(round));

  REQUIRE(!outbox->entries.empty());
  const Json::Value payload = payloadOf(outbox->entries[0]);
  CHECK(payload["ttlSeconds"].asUInt64() == static_cast<uint64_t>(kAlertTtlSeconds));
  CHECK(payload["expiresAt"].asInt64() == (kNow + kAlertTtlSeconds) * 1000);
}

TEST_CASE("the collapse key of an alert is the hex of its delivery id, and its msg id is the same hex")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  NotificationAlertRound round;
  round.inputs = {alertFor(42)};
  round.notificationIds = {901};

  drogon::sync_wait(writerOver(outbox).write(round));

  REQUIRE(!outbox->entries.empty());
  const Json::Value payload = payloadOf(outbox->entries[0]);
  const std::string deliveryId = base64::decode(payload["deliveryId"].asString()).value();
  CHECK(payload["collapseId"].asString() == push_delivery_wire::hexOf(deliveryId));
  CHECK(outbox->entries[0].eventId == std::string(push_delivery_wire::kEventIdPrefix) +
                                          push_delivery_wire::hexOf(deliveryId));
}

TEST_CASE("a user without devices and a row without a user write no alert")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  NotificationAlertRound round;
  round.inputs = {alertFor(7), alertFor(0)};
  round.notificationIds = {901, 902};

  drogon::sync_wait(writerOver(outbox).write(round));

  CHECK(outbox->entries.empty());
}
