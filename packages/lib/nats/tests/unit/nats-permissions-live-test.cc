#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <nats/nats-bus.hxx>
#include <nats/nats-permissions.hxx>
#include <nats/nats-subject.hxx>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <trantor/utils/Logger.h>
#include <vector>

namespace
{
using nats_permissions::ServicePermissions;

constexpr int64_t kSecondNs = 1000LL * 1000 * 1000;

std::string env(const char* name)
{
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

bool unconfigured()
{
  return env("ARGUS_NATS_PERMISSIONS_URL").empty() ||
         env("ARGUS_NATS_PERMISSIONS_SEEDS").empty();
}

std::filesystem::path seedsDir()
{
  return env("ARGUS_NATS_PERMISSIONS_SEEDS");
}

std::string publicKeyOf(std::string_view service)
{
  std::ifstream in(seedsDir() / (std::string(service) + ".pub"));
  std::string key;
  in >> key;
  return key;
}

NatsBus::Options optionsFor(std::string_view service)
{
  NatsBus::Options options;
  options.url = env("ARGUS_NATS_PERMISSIONS_URL");
  options.nkey = publicKeyOf(service);
  options.seedFile = (seedsDir() / (std::string(service) + ".seed")).string();
  options.inboxPrefix = nats_permissions::inboxPrefix(service);
  options.reconnectWaitMs = 200;
  options.maxReconnects = 3;
  return options;
}

NatsBus::Options sharedOptions()
{
  NatsBus::Options options;
  options.url = env("ARGUS_NATS_PERMISSIONS_URL");
  options.user = "argus";
  options.password = env("ARGUS_NATS_PERMISSIONS_LEGACY");
  options.reconnectWaitMs = 200;
  options.maxReconnects = 3;
  return options;
}

struct CapturedLog
{
  std::mutex mutex;
  std::vector<std::string> lines;
};

CapturedLog& capturedLog()
{
  static CapturedLog log;
  return log;
}

void captureLogs()
{
  trantor::Logger::setOutputFunction(
      [](const char* message, const uint64_t length) {
        {
          std::scoped_lock lock(capturedLog().mutex);
          capturedLog().lines.emplace_back(message,
                                           static_cast<size_t>(length));
        }
        std::cout.write(message, static_cast<std::streamsize>(length));
      },
      [] { std::cout << std::flush; });
}

int logLinesWith(std::string_view needle)
{
  std::scoped_lock lock(capturedLog().mutex);
  return static_cast<int>(
      std::ranges::count_if(capturedLog().lines,
                            [needle](const std::string& line) {
                              return line.find(needle) != std::string::npos;
                            }));
}

bool awaitLogLine(std::string_view needle)
{
  for (int attempt = 0; attempt < 150; ++attempt) {
    if (logLinesWith(needle) > 0)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void settle()
{
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

const std::map<std::string_view, std::vector<std::string>>& streamSubjects()
{
  using namespace nats_subject;
  static const std::map<std::string_view, std::vector<std::string>> streams{
      {kCameraStream, {kCameraChange, kCameraObjectDetected}},
      {kGuardStream, {kGuardSubjectFilter}},
      {kNotificationChangeStream, {kNotificationChange}},
      {kNotificationDeliveryStream, {kNotificationDelivery}},
      {kNotificationVerdictStream, {kNotificationResponseVerdict}},
      {kProductivityChangeStream, {kProductivityChange}},
      {kProductivityActionStream, {kProductivityUserAction}},
      {kIdentityChangeStream, {kIdentityChange, kIdentityUserAction}},
      {kAuthChangeStream, {kAuthUserAction}},
      {kAuthSessionStream, {kAuthSession}},
      {kSettingsModuleStream, {kSettingsModule}},
      {kSettingsActionStream, {kSettingsUserAction}},
      {kSettingsChangeStream, {kSettingsChange}},
  };
  return streams;
}

NatsBus::StreamInput streamInput(std::string_view name, int64_t maxAgeSeconds)
{
  const auto subjects = streamSubjects().find(name);
  REQUIRE_MESSAGE(subjects != streamSubjects().end(),
                  "no subjects for " << name);
  return {.name = std::string(name),
          .subjects = subjects->second,
          .maxAgeNs = maxAgeSeconds * kSecondNs,
          .duplicatesNs = 60 * kSecondNs};
}

struct Counter
{
  std::atomic<int> count{0};
};

NatsBus::DurableInput durableFor(const nats_permissions::Consumer& consumer,
                                 const std::shared_ptr<Counter>& counter)
{
  return {.stream = std::string(consumer.stream),
          .durable = std::string(consumer.name),
          .subject = std::string(consumer.subject),
          .deliverAll = true,
          .deliverLastPerSubject = false,
          .recreateOnPolicyChange = false,
          .quiet = false,
          .maxDeliver = 5,
          .maxAckPending = NatsBus::kDefaultMaxAckPending,
          .handler = [counter](const NatsBus::DurableMessage&,
                               const NatsBus::DurableSettlement& settlement) {
            counter->count.fetch_add(1);
            settlement.ack();
          }};
}

bool awaitCount(const Counter& counter, int at_least)
{
  for (int attempt = 0; attempt < 150; ++attempt) {
    if (counter.count.load() >= at_least)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

using Fleet = std::map<std::string_view, std::unique_ptr<NatsBus>>;

Fleet connectFleet()
{
  Fleet fleet;
  for (const auto& row : nats_permissions::table()) {
    auto bus = std::make_unique<NatsBus>();
    CHECK_MESSAGE(bus->connect(optionsFor(row.service)),
                  row.service << " could not connect with its own nkey");
    fleet.emplace(row.service, std::move(bus));
  }
  return fleet;
}

void ensureStreams(const ServicePermissions& row, NatsBus& bus)
{
  for (const auto stream : row.streams) {
    CHECK_MESSAGE(bus.ensureStream(streamInput(stream, 60)),
                  row.service << " could not create " << stream);
    CHECK_MESSAGE(bus.ensureStream(streamInput(stream, 120)),
                  row.service << " could not reconcile " << stream);
  }
}

void publishAll(const ServicePermissions& row, NatsBus& bus)
{
  for (const auto subject : row.stores)
    CHECK_MESSAGE(bus.publishWithMsgId({.subject = std::string(subject),
                                        .payload = "{}",
                                        .msgId = std::string(row.service) +
                                                 "-" + std::string(subject)}),
                  row.service << " could not store on " << subject);
  for (const auto subject : row.publishes)
    CHECK_MESSAGE(bus.publish(subject, "{}"),
                  row.service << " could not publish on " << subject);
}

void subscribeAll(const ServicePermissions& row, NatsBus& bus)
{
  for (const auto subject : row.subscribes) {
    if (subject.starts_with('$'))
      continue;
    CHECK_MESSAGE(bus.subscribe(std::string(subject),
                                [](std::string_view, std::string_view) {})
                      .has_value(),
                  row.service << " could not subscribe to " << subject);
  }
}

void drainConsumers(const ServicePermissions& row, NatsBus& bus)
{
  for (const auto& consumer : row.consumers) {
    auto counter = std::make_shared<Counter>();
    const auto id = bus.subscribeDurable(durableFor(consumer, counter));
    REQUIRE_MESSAGE(id.has_value(),
                    row.service << " could not bind " << consumer.name);
    CHECK_MESSAGE(awaitCount(*counter, 1),
                  consumer.name << " received nothing on " << consumer.stream);
    bus.unsubscribe(id.value_or(0));
    if (!consumer.recreates)
      continue;
    auto again = std::make_shared<Counter>();
    auto lastOnly = durableFor(consumer, again);
    lastOnly.deliverAll = false;
    lastOnly.deliverLastPerSubject = true;
    lastOnly.recreateOnPolicyChange = true;
    std::optional<uint64_t> recreated;
    for (int attempt = 0; attempt < 40 && !recreated; ++attempt) {
      recreated = bus.subscribeDurable(lastOnly);
      if (!recreated)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE_MESSAGE(recreated.has_value(),
                    row.service << " could not recreate " << consumer.name);
    CHECK_MESSAGE(awaitCount(*again, 1),
                  consumer.name << " received nothing after it was recreated");
    bus.unsubscribe(recreated.value_or(0));
  }
}

}

TEST_CASE("every row's own operations pass the broker, and none is refused" *
          doctest::skip(unconfigured()))
{
  captureLogs();
  Fleet fleet = connectFleet();
  const auto& table = nats_permissions::table();
  for (const auto& row : table)
    ensureStreams(row, *fleet.at(row.service));
  for (const auto& row : table)
    publishAll(row, *fleet.at(row.service));
  for (const auto& row : table)
    subscribeAll(row, *fleet.at(row.service));
  settle();
  for (const auto& row : table)
    drainConsumers(row, *fleet.at(row.service));
  settle();
  CHECK(logLinesWith("Permissions Violation") == 0);
  CHECK(logLinesWith("Authorization Violation") == 0);
  for (auto& [service, bus] : fleet)
    bus->drain();
}

TEST_CASE("a service is refused outside its row and outside its own inbox" *
          doctest::skip(unconfigured()))
{
  captureLogs();
  NatsBus shared;
  REQUIRE(shared.connect(sharedOptions()));
  REQUIRE(shared.ensureStream(streamInput(nats_subject::kCameraStream, 60)));

  Fleet fleet = connectFleet();
  const auto& table = nats_permissions::table();
  for (size_t index = 0; index < table.size(); ++index) {
    const auto& row = table[index];
    const auto& other = table[(index + 1) % table.size()];
    NatsBus& bus = *fleet.at(row.service);
    const std::string foreignInbox =
        nats_permissions::inboxPrefix(other.service) + ".probe";
    REQUIRE(
        bus.subscribe(foreignInbox, [](std::string_view, std::string_view) {})
            .has_value());
    CHECK_MESSAGE(awaitLogLine("Subscription to \"" + foreignInbox + "\""),
                  row.service << " could subscribe inside " << other.service
                              << "'s inbox");
    const std::string forbidden =
        "argus.forbidden.v1." + std::string(row.service);
    CHECK(bus.publish(forbidden, "{}"));
    CHECK_MESSAGE(awaitLogLine("Publish to \"" + forbidden + "\""),
                  row.service << " could publish " << forbidden);
  }

  NatsBus& relay = *fleet.at("relay");
  const std::string relayInbox =
      nats_permissions::inboxPrefix("relay") + ".probe";
  REQUIRE(relay.subscribe(relayInbox, [](std::string_view, std::string_view) {})
              .has_value());
  CHECK_MESSAGE(awaitLogLine("Subscription to \"" + relayInbox + "\""),
                "relay, which reads no JetStream, could subscribe to an inbox");

  NatsBus& auth = *fleet.at("auth");
  CHECK_FALSE(auth.ensureStream(streamInput(nats_subject::kCameraStream, 90)));
  auto counter = std::make_shared<Counter>();
  CHECK_FALSE(
      auth.subscribeDurable(durableFor({.stream = nats_subject::kCameraStream,
                                        .name = "argus-sync-camera",
                                        .subject = nats_subject::kCameraChange,
                                        .recreates = false},
                                       counter))
          .has_value());

  NatsBus stranger;
  NatsBus::Options unknown = optionsFor("auth");
  unknown.nkey = "UDXU4RCSJNZOIQHZNWXHXORDPRTGNJAHAHFRGZNEEJCPQTT2M7NLCNF4";
  unknown.maxReconnects = 1;
  CHECK_FALSE(stranger.connect(unknown));
  stranger.drain();

  for (auto& [service, bus] : fleet)
    bus->drain();
  shared.drain();
}

TEST_CASE("the shared user keeps working until every service has switched" *
          doctest::skip(unconfigured()))
{
  captureLogs();
  NatsBus shared;
  REQUIRE(shared.connect(sharedOptions()));
  CHECK(shared.publish("argus.anything.v1.probe", "{}"));
  CHECK(shared.ensureStream(
      streamInput(nats_subject::kSettingsModuleStream, 60)));
  settle();
  CHECK(logLinesWith("Permissions Violation for Publish to \"argus.anything") ==
        0);
  shared.drain();
}

TEST_CASE(
    "a durable the shared user bound moves under the service's inbox when "
    "the service switches" *
    doctest::skip(unconfigured()))
{
  captureLogs();
  const int violationsBefore = logLinesWith("Permissions Violation");
  NatsBus shared;
  REQUIRE(shared.connect(sharedOptions()));
  REQUIRE(shared.ensureStream(streamInput(nats_subject::kCameraStream, 60)));
  const nats_permissions::Consumer consumer{.stream =
                                                nats_subject::kCameraStream,
                                            .name = "argus-sync-camera",
                                            .subject =
                                                nats_subject::kCameraChange,
                                            .recreates = false};
  auto legacyDeliveries = std::make_shared<Counter>();
  std::optional<uint64_t> legacy;
  for (int attempt = 0; attempt < 40 && !legacy; ++attempt) {
    legacy = shared.subscribeDurable(durableFor(consumer, legacyDeliveries));
    if (!legacy)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(legacy.has_value());
  shared.unsubscribe(legacy.value_or(0));
  settle();
  REQUIRE(shared.publishWithMsgId({.subject = nats_subject::kCameraChange,
                                   .payload = "{}",
                                   .msgId = "switch-probe"}));

  NatsBus sync;
  REQUIRE(sync.connect(optionsFor("sync")));
  auto switchedDeliveries = std::make_shared<Counter>();
  std::optional<uint64_t> switched;
  for (int attempt = 0; attempt < 40 && !switched; ++attempt) {
    switched = sync.subscribeDurable(durableFor(consumer, switchedDeliveries));
    if (!switched)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(switched.has_value());
  CHECK(awaitCount(*switchedDeliveries, 1));
  settle();
  CHECK(logLinesWith("Permissions Violation") == violationsBefore);
  sync.unsubscribe(switched.value_or(0));
  sync.drain();
  shared.drain();
}
