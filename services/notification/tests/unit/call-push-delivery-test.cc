#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/call/services/push-delivery.hxx>
#include <shared/services/push-delivery/push-delivery-wire.hxx>
#include "../support/push-open.hxx"
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
constexpr int64_t kCancelTtlSeconds = 60;

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

class OneFcmDevice final : public PushDeviceDirectory
{
public:
  drogon::Task<std::vector<PushDevice>> devicesOf(int64_t, drogon::orm::DbClient*) const override
  {
    co_return std::vector<PushDevice>{{.provider = PushProvider::Fcm,
                                       .token = "fcm-token-0001",
                                       .publicKey = base64UrlOf(fromHex(kRecipientPublicHex)),
                                       .keyId = "key-0001"}};
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

CallSchema ringingCall(int64_t expiresAt)
{
  CallSchema call;
  call.id = 17;
  call.userId = 42;
  call.dedupeKey = "guard:episode:9";
  call.urgency = "critical";
  call.lang = "es";
  call.title = "Puerta";
  call.summary = "Hay alguien en la puerta";
  call.createdAt = kNow;
  call.expiresAt = expiresAt;
  return call;
}

CallPushDelivery deliveryOver(const std::shared_ptr<RecordingOutbox>& outbox)
{
  return CallPushDelivery(CallPushDeliveryDependencies{.devices = std::make_shared<OneFcmDevice>(),
                                                       .outbox = outbox,
                                                       .clock = [] { return kNow; }});
}

Json::Value payloadOf(const PushDeliveryRecord& entry)
{
  return json_util::fromString(entry.payload);
}
}

TEST_CASE("a ring is recorded with the end of its ring as expiresAt in epoch milliseconds")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  const CallSchema call = ringingCall(kNow + 120);

  const int64_t recorded = drogon::sync_wait(deliveryOver(outbox).ring(call, nullptr));

  CHECK(recorded == 1);
  REQUIRE(outbox->entries.size() == 1);
  const Json::Value payload = payloadOf(outbox->entries[0]);
  CHECK(payload["expiresAt"].asInt64() == (kNow + 120) * 1000);
  CHECK(payload["ttlSeconds"].asUInt64() == 120);
  CHECK(payload["collapseId"].asString() == call.callId());
  CHECK(payload["kind"].asString() == "call");
}

TEST_CASE("a cancel is recorded with expiresAt sixty seconds after it is sealed")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  const CallSchema call = ringingCall(kNow + 900);

  const int64_t recorded = drogon::sync_wait(deliveryOver(outbox).cancel(call, nullptr));

  CHECK(recorded == 1);
  REQUIRE(outbox->entries.size() == 1);
  const Json::Value payload = payloadOf(outbox->entries[0]);
  CHECK(payload["expiresAt"].asInt64() == (kNow + kCancelTtlSeconds) * 1000);
  CHECK(payload["ttlSeconds"].asUInt64() == static_cast<uint64_t>(kCancelTtlSeconds));
}

TEST_CASE("a delivery is recorded under the msg id of its delivery id, once per device")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  static_cast<void>(drogon::sync_wait(deliveryOver(outbox).ring(ringingCall(kNow + 120), nullptr)));
  REQUIRE(outbox->entries.size() == 1);

  const std::string eventId = outbox->entries[0].eventId;
  const Json::Value payload = payloadOf(outbox->entries[0]);
  const auto decodedDeliveryId = base64::decode(payload["deliveryId"].asString());
  if (!decodedDeliveryId) {
    FAIL("the delivery id is not base64");
    return;
  }
  const std::string& deliveryId = *decodedDeliveryId;

  CHECK(eventId == std::string(push_delivery_wire::kEventIdPrefix) + push_delivery_wire::hexOf(deliveryId));
  CHECK(payload["token"].asString() == "fcm-token-0001");
  CHECK(payload["provider"].asString() == "fcm");
}

TEST_CASE("a ring seals a call plaintext with its title, body and deep link, opened with the device key")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  const CallSchema call = ringingCall(kNow + 120);
  static_cast<void>(drogon::sync_wait(deliveryOver(outbox).ring(call, nullptr)));
  REQUIRE(outbox->entries.size() == 1);

  const Json::Value plain = push_open::plaintextOf(outbox->entries[0].payload, "key-0001");

  CHECK(plain["kind"].asString() == "call");
  CHECK(plain["callId"].asString() == call.callId());
  CHECK(plain["episode"].asString() == "guard:episode:9");
  CHECK(plain["title"].asString() == "Argus te está llamando");
  CHECK(plain["body"].asString() == "Abre Argus para contestar.");
  CHECK(plain["deepLink"].asString() == "argus://call?callId=" + call.callId());
  CHECK(plain["exp"].asInt64() == kNow + 120);
  CHECK_FALSE(plain.isMember("cancel"));
}

TEST_CASE("a cancel seals the same call plaintext with cancel set, and the wrong key opens nothing")
{
  const auto outbox = std::make_shared<RecordingOutbox>();
  const CallSchema call = ringingCall(kNow + 900);
  static_cast<void>(drogon::sync_wait(deliveryOver(outbox).cancel(call, nullptr)));
  REQUIRE(outbox->entries.size() == 1);

  const Json::Value plain = push_open::plaintextOf(outbox->entries[0].payload, "key-0001");

  CHECK(plain["cancel"].asBool());
  CHECK(plain["exp"].asInt64() == kNow + kCancelTtlSeconds);
  CHECK(plain["callId"].asString() == call.callId());
  CHECK(plain["title"].asString() == "Argus te está llamando");
  CHECK(plain["deepLink"].asString() == "argus://call?callId=" + call.callId());
  CHECK(push_open::plaintextOf(outbox->entries[0].payload, "key-0002").isNull());
}
