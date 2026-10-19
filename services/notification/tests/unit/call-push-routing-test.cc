#include <doctest/doctest.h>

#include <feature/call/services/call-push-routing.hxx>

#include <vector>

TEST_CASE("a call to an iOS device goes to apns_voip only, never to apns")
{
  const std::vector<PushDevice> ios{
      {.provider = PushProvider::Apns, .token = "apns-alert-token", .publicKey = "k", .keyId = "kid", .sessionId = ""},
      {.provider = PushProvider::ApnsVoip, .token = "apns-voip-token", .publicKey = "k", .keyId = "kid", .sessionId = ""}};
  const auto routed = call_push_routing::devicesForCall(ios);
  REQUIRE(routed.size() == 1);
  CHECK(routed.front().provider == PushProvider::ApnsVoip);
  CHECK(routed.front().token == "apns-voip-token");
  for (const auto& device : routed)
    CHECK(device.provider != PushProvider::Apns);
}

TEST_CASE("a call to an Android device goes to fcm and an iOS device with only apns gets no call")
{
  const std::vector<PushDevice> mixed{
      {.provider = PushProvider::Fcm, .token = "fcm-token", .publicKey = "k", .keyId = "kid", .sessionId = ""},
      {.provider = PushProvider::Apns, .token = "apns-alert-token", .publicKey = "k", .keyId = "kid", .sessionId = ""}};
  const auto routed = call_push_routing::devicesForCall(mixed);
  REQUIRE(routed.size() == 1);
  CHECK(routed.front().provider == PushProvider::Fcm);
  CHECK(call_push_routing::devicesForCall({mixed[1]}).empty());
}

TEST_CASE("a household call rings an Android phone through fcm and an iPhone through apns_voip side by side")
{
  const std::vector<PushDevice> household{
      {.provider = PushProvider::Fcm, .token = "android-fcm", .publicKey = "k", .keyId = "kid", .sessionId = "s1"},
      {.provider = PushProvider::Apns, .token = "iphone-alert", .publicKey = "k", .keyId = "kid", .sessionId = "s2"},
      {.provider = PushProvider::ApnsVoip, .token = "iphone-voip", .publicKey = "k", .keyId = "kid", .sessionId = "s2"}};
  const auto routed = call_push_routing::devicesForCall(household);
  REQUIRE(routed.size() == 2);
  CHECK(routed[0].provider == PushProvider::Fcm);
  CHECK(routed[0].token == "android-fcm");
  CHECK(routed[1].provider == PushProvider::ApnsVoip);
  CHECK(routed[1].token == "iphone-voip");
  const auto others = call_push_routing::devicesExceptSession(routed, "s1");
  REQUIRE(others.size() == 1);
  CHECK(others.front().provider == PushProvider::ApnsVoip);
}
