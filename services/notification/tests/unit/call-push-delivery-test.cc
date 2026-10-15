#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/call/infra/nats-call-push-sink.hxx>
#include <feature/call/services/push-delivery.hxx>
#include <text/base64.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

class OneFcmDevice final : public CallPushDeviceDirectory
{
public:
  drogon::Task<std::vector<CallPushDevice>> devicesOf(int64_t) const override
  {
    co_return std::vector<CallPushDevice>{{.provider = PushProvider::Fcm,
                                           .token = "fcm-token-0001",
                                           .publicKey = base64UrlOf(fromHex(kRecipientPublicHex)),
                                           .keyId = "key-0001"}};
  }
};

class RecordingCallSink final : public CallPushSink
{
public:
  bool publish(const CallPushMessage& message) const override
  {
    messages.push_back(message);
    return true;
  }

  mutable std::vector<CallPushMessage> messages;
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

CallPushDelivery deliveryOver(const std::shared_ptr<RecordingCallSink>& sink)
{
  return CallPushDelivery(std::make_shared<OneFcmDevice>(), sink, [] { return kNow; });
}
}

TEST_CASE("a ring is published with the end of its ring as expiresAt in epoch milliseconds")
{
  const auto sink = std::make_shared<RecordingCallSink>();
  const CallSchema call = ringingCall(kNow + 120);

  const int64_t published = drogon::sync_wait(deliveryOver(sink).ring(call));

  CHECK(published == 1);
  REQUIRE(sink->messages.size() == 1);
  CHECK(sink->messages[0].expiresAtMs == (kNow + 120) * 1000);
  CHECK(sink->messages[0].envelope.ttlSeconds == 120);
  CHECK(sink->messages[0].envelope.collapseId == call.callId());
}

TEST_CASE("a cancel is published with expiresAt sixty seconds after it is sealed")
{
  const auto sink = std::make_shared<RecordingCallSink>();
  const CallSchema call = ringingCall(kNow + 900);

  const int64_t published = drogon::sync_wait(deliveryOver(sink).cancel(call));

  CHECK(published == 1);
  REQUIRE(sink->messages.size() == 1);
  CHECK(sink->messages[0].expiresAtMs == (kNow + kCancelTtlSeconds) * 1000);
  CHECK(sink->messages[0].envelope.ttlSeconds == kCancelTtlSeconds);
}

TEST_CASE("the call wire payload carries expiresAt beside the sealed envelope")
{
  const auto sink = std::make_shared<RecordingCallSink>();
  static_cast<void>(drogon::sync_wait(deliveryOver(sink).ring(ringingCall(kNow + 120))));
  REQUIRE(sink->messages.size() == 1);

  const Json::Value json = call_push_wire::toJson(sink->messages[0]);

  CHECK(json["expiresAt"].isIntegral());
  CHECK(json["expiresAt"].asInt64() == (kNow + 120) * 1000);
  CHECK(json["kind"].asString() == "call");
  CHECK(json["provider"].asString() == "fcm");
  CHECK(json["collapseId"].asString() == sink->messages[0].envelope.collapseId);
  CHECK(json["token"].asString() == "fcm-token-0001");
}
