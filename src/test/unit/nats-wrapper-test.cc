#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using nats_subject::SubjectKind;
using nats_subject::isValidSubject;

} // namespace

TEST_CASE("publish subjects accept plain dotted tokens only")
{
  CHECK(isValidSubject("argus.sync.v1.change", SubjectKind::Publish));
  CHECK(isValidSubject("argus.sync.v1.change", SubjectKind::Subscribe));
  CHECK(isValidSubject("argus.camera.v1.snapshot", SubjectKind::Publish));
  CHECK(isValidSubject("argus.a-1_B.v1.e", SubjectKind::Publish));

  CHECK_FALSE(isValidSubject("", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus..change", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus.sync.v1.", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus sync v1 change", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus.sync.v1.changé", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus.>.v1.change", SubjectKind::Publish));
  CHECK_FALSE(isValidSubject("argus.*.v1.change", SubjectKind::Publish));
}

TEST_CASE("subscribe subjects allow NATS wildcards")
{
  CHECK(isValidSubject("argus.>.v1.change", SubjectKind::Subscribe));
  CHECK(isValidSubject("argus.*.v1.change", SubjectKind::Subscribe));
  CHECK(isValidSubject(">", SubjectKind::Subscribe));
  CHECK(isValidSubject("argus.>", SubjectKind::Subscribe));

  CHECK_FALSE(isValidSubject("argus.v1.*.change", SubjectKind::Publish));
}

TEST_CASE("frozen sync subjects keep their contract spelling")
{
  CHECK(std::string(nats_subject::kSyncChange) == "argus.sync.v1.change");
  CHECK(std::string(nats_subject::kSyncChangeWildcard) ==
        "argus.>.v1.change");
  CHECK(isValidSubject(nats_subject::kSyncChange, SubjectKind::Publish));
  CHECK(isValidSubject(nats_subject::kSyncChangeWildcard,
                       SubjectKind::Subscribe));
}

TEST_CASE("options fall back to sane defaults without config")
{
  const auto options = NatsBus::optionsFromConfig();
  CHECK(options.url == "nats://127.0.0.1:4222");
  CHECK(options.reconnectWaitMs == 2000);
  CHECK(options.maxReconnects == 60);
}

TEST_CASE("handler registration without a server stays pending")
{
  NatsBus bus;
  REQUIRE_FALSE(bus.isConnected());

  std::atomic<int> calls{0};
  const auto first =
      bus.subscribe(nats_subject::kSyncChange, [&calls](std::string_view) {
        ++calls;
      });
  CHECK(first.has_value());

  const auto second = bus.subscribe("argus.*.v1.change",
                                    [](std::string_view) {});
  CHECK(second.has_value());
  CHECK(*second != *first);

  CHECK_FALSE(bus.publish(nats_subject::kSyncChange, "{}"));

  CHECK(bus.unsubscribe(*second));

  bus.drain();
  CHECK_FALSE(bus.isConnected());

  const auto afterDrain =
      bus.subscribe(nats_subject::kSyncChange, [](std::string_view) {});
  CHECK_FALSE(afterDrain.has_value());

  bus.drain();
  CHECK_FALSE(bus.isConnected());
}

TEST_CASE("connect to a closed endpoint fails without crashing")
{
  NatsBus bus;
  NatsBus::Options options;
  options.url = "nats://127.0.0.1:59999";
  options.reconnectWaitMs = 10;
  options.maxReconnects = 1;

  CHECK_FALSE(bus.connect(options));
  CHECK_FALSE(bus.isConnected());
  CHECK_FALSE(bus.publish(nats_subject::kSyncChange, "{}"));

  bus.drain();
  bus.drain();
}

TEST_CASE("live roundtrip against a running nats-server")
{
  const char* url = std::getenv("ARGUS_TEST_NATS_URL");
  if (url == nullptr || std::string(url).empty()) {
    std::cout << "SKIP: ARGUS_TEST_NATS_URL not provided; start a local "
                 "nats-server and export ARGUS_TEST_NATS_URL to run the "
                 "live NATS roundtrip\n";
    return;
  }

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  REQUIRE(bus.connect(options));
  CHECK(bus.isConnected());

  std::atomic<int> calls{0};
  std::string received;
  const auto id = bus.subscribe(
      nats_subject::kSyncChange, [&](std::string_view payload) {
        received = std::string(payload);
        ++calls;
      });
  REQUIRE(id.has_value());

  const auto wildcard = bus.subscribe(
      nats_subject::kSyncChangeWildcard, [](std::string_view) {});
  CHECK(wildcard.has_value());

  Json::Value payload;
  payload["operation"] = 4;
  payload["option"] = "camera";
  Json::Value info;
  info["id"] = 1;
  payload["info"] = info;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string body = Json::writeString(builder, payload);

  bool delivered = false;
  for (int attempt = 0; attempt < 20; ++attempt) {
    delivered = bus.publish(nats_subject::kSyncChange, body);
    if (delivered)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(delivered);

  for (int i = 0; i < 50 && calls.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(calls.load() >= 1);
  CHECK(received == body);

  CHECK(bus.unsubscribe(*id));
  bus.drain();
  CHECK_FALSE(bus.isConnected());
}