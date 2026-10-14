#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/call/services/call-push-routing.hxx>

#include <vector>

TEST_CASE("a call to an iOS device goes to apns_voip only, never to apns")
{
  const std::vector<CallPushDevice> ios{
      {.provider = PushProvider::Apns, .token = "apns-alert-token", .publicKey = "k", .keyId = "kid"},
      {.provider = PushProvider::ApnsVoip, .token = "apns-voip-token", .publicKey = "k", .keyId = "kid"}};
  const auto routed = call_push_routing::devicesForCall(ios);
  REQUIRE(routed.size() == 1);
  CHECK(routed.front().provider == PushProvider::ApnsVoip);
  CHECK(routed.front().token == "apns-voip-token");
  for (const auto& device : routed)
    CHECK(device.provider != PushProvider::Apns);
}

TEST_CASE("a call to an Android device goes to fcm and an iOS device with only apns gets no call")
{
  const std::vector<CallPushDevice> mixed{
      {.provider = PushProvider::Fcm, .token = "fcm-token", .publicKey = "k", .keyId = "kid"},
      {.provider = PushProvider::Apns, .token = "apns-alert-token", .publicKey = "k", .keyId = "kid"}};
  const auto routed = call_push_routing::devicesForCall(mixed);
  REQUIRE(routed.size() == 1);
  CHECK(routed.front().provider == PushProvider::Fcm);
  CHECK(call_push_routing::devicesForCall({mixed[1]}).empty());
}
