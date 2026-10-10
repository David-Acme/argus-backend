#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include <trantor/utils/Logger.h>
#include <stdexcept>

template <typename T>
const T& requireValue(const std::optional<T>& value)
{
  if (!value)
    throw std::runtime_error("test: expected a value");
  return *value;
}

namespace
{

using nats_subject::SubjectKind;
using nats_subject::isValidSubject;

int streamCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

std::string isolatedStream()
{
  return "argus-test-stream-" + std::to_string(::getpid()) + "-" +
         std::to_string(streamCounter());
}

std::string isolatedSubject(const std::string& stream)
{
  return stream + ".events";
}

struct Deliveries
{
  std::mutex mutex;
  std::vector<std::string> payloads;
  std::vector<std::string> msgIds;
};

NatsBus::DurableInput collectingInto(NatsBus::DurableInput input,
                                     Deliveries& deliveries)
{
  input.handler = [&deliveries](const NatsBus::DurableMessage& message,
                                const NatsBus::DurableSettlement& settlement) {
    {
      std::lock_guard lock(deliveries.mutex);
      deliveries.payloads.emplace_back(message.payload);
      deliveries.msgIds.emplace_back(message.msgId);
    }
    settlement.ack();
  };
  return input;
}

std::vector<std::string> awaitPayloads(Deliveries& deliveries, size_t count)
{
  for (int attempt = 0; attempt < 150; ++attempt) {
    {
      std::lock_guard lock(deliveries.mutex);
      if (deliveries.payloads.size() >= count)
        return deliveries.payloads;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard lock(deliveries.mutex);
  return deliveries.payloads;
}

std::vector<std::string> msgIdsOf(Deliveries& deliveries)
{
  std::lock_guard lock(deliveries.mutex);
  return deliveries.msgIds;
}

std::optional<uint64_t> attachWithin(NatsBus& bus,
                                     const NatsBus::DurableInput& input)
{
  for (int attempt = 0; attempt < 40; ++attempt) {
    if (const auto id = bus.subscribeDurable(input))
      return id;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::nullopt;
}

}

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

TEST_CASE("subscribe subjects allow NATS wildcards with > tail-only")
{
  CHECK(isValidSubject("argus.*.v1.change", SubjectKind::Subscribe));
  CHECK(isValidSubject(">", SubjectKind::Subscribe));
  CHECK(isValidSubject("argus.>", SubjectKind::Subscribe));

  CHECK_FALSE(isValidSubject("argus.>.v1.change", SubjectKind::Subscribe));
  CHECK_FALSE(isValidSubject("argus.v1.*.change", SubjectKind::Publish));
}

TEST_CASE("the sync change subject keeps its contract spelling")
{
  CHECK(std::string(nats_subject::kSyncChange) == "argus.sync.v1.change");
  CHECK(isValidSubject(nats_subject::kSyncChange, SubjectKind::Publish));
}

TEST_CASE("every change stream carries the name its consumer subscribes to")
{
  CHECK(std::string(nats_subject::kCameraStream) == "ARGUS_CAMERA");
  CHECK(std::string(nats_subject::kNotificationChangeStream) ==
        "ARGUS_NOTIFICATION_CHANGE");
  CHECK(std::string(nats_subject::kProductivityChangeStream) ==
        "ARGUS_PRODUCTIVITY_CHANGE");
  CHECK(std::string(nats_subject::kIdentityChangeStream) ==
        "ARGUS_IDENTITY_CHANGE");
  CHECK(std::string(nats_subject::kNotificationDeliveryStream) ==
        "ARGUS_NOTIFICATION");
  CHECK(std::string(nats_subject::kGuardStream) == "ARGUS_GUARD");

  CHECK(isValidSubject(nats_subject::kCameraChange, SubjectKind::Subscribe));
  CHECK(isValidSubject(nats_subject::kNotificationChange,
                       SubjectKind::Subscribe));
  CHECK(isValidSubject(nats_subject::kProductivityChange,
                       SubjectKind::Subscribe));
  CHECK(isValidSubject(nats_subject::kIdentityChange,
                       SubjectKind::Subscribe));
  CHECK(isValidSubject(nats_subject::kIdentityUserAction,
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
      bus.subscribe(
          nats_subject::kSyncChange,
          [&calls](std::string_view, std::string_view) { ++calls; });
  CHECK(first.has_value());

  const auto second =
      bus.subscribe("argus.*.v1.change",
                    [](std::string_view, std::string_view) {});
  CHECK(second.has_value());
  CHECK(*second != *first);

  CHECK_FALSE(bus.publish(nats_subject::kSyncChange, "{}"));

  CHECK(bus.unsubscribe(*second));

  bus.drain();
  CHECK_FALSE(bus.isConnected());

  const auto afterDrain =
      bus.subscribe(
          nats_subject::kSyncChange, [](std::string_view, std::string_view) {});
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

TEST_CASE("live roundtrip against a running nats-server" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  REQUIRE(bus.connect(options));
  CHECK(bus.isConnected());

  std::atomic<int> calls{0};
  std::string received;
  const auto id = bus.subscribe(
      nats_subject::kSyncChange,
      [&](std::string_view, std::string_view payload) {
        received = std::string(payload);
        ++calls;
      });
  REQUIRE(id.has_value());

  const auto wildcard = bus.subscribe(
      "argus.*.v1.change", [](std::string_view, std::string_view) {});
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

TEST_CASE("ensureStream reconciles an existing stream instead of assuming it" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  const NatsBus::StreamInput create{.name = stream,
                                    .subjects = {subject},
                                    .maxAgeNs = 60LL * 1000000000,
                                    .duplicatesNs = 60LL * 1000000000};
  REQUIRE(bus.ensureStream(create));

  const auto created = bus.streamInfo(stream);
  REQUIRE(created.has_value());
  REQUIRE(created->subjects.size() == 1);
  CHECK(created->subjects.front() == subject);
  CHECK(created->maxAgeNs == 60LL * 1000000000);

  CHECK(bus.ensureStream(create));

  const NatsBus::StreamInput aged{.name = stream,
                                  .subjects = {subject},
                                  .maxAgeNs = 120LL * 1000000000,
                                  .duplicatesNs = 60LL * 1000000000};
  CHECK(bus.ensureStream(aged));
  const auto updated = bus.streamInfo(stream);
  REQUIRE(updated.has_value());
  CHECK(updated->maxAgeNs == 120LL * 1000000000);

  const NatsBus::StreamInput foreign{.name = stream,
                                     .subjects = {subject + "-other"},
                                     .maxAgeNs = 120LL * 1000000000,
                                     .duplicatesNs = 60LL * 1000000000};
  CHECK_FALSE(bus.ensureStream(foreign));
  const auto intact = bus.streamInfo(stream);
  REQUIRE(intact.has_value());
  REQUIRE(intact->subjects.size() == 1);
  CHECK(intact->subjects.front() == subject);

  bus.drain();
}

TEST_CASE("a durable consumer outlives the subscriber that bound it" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();
  NatsBus::Options options;
  options.url = url;

  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  const NatsBus::DurableInput feed{
      .stream = stream,
      .durable = stream + "-durable",
      .subject = subject,
      .deliverAll = false,
      .maxDeliver = 5,
      .maxAckPending = NatsBus::kDefaultMaxAckPending,
      .handler = {}};
  const auto publish = [&subject](NatsBus& bus, const std::string& body) {
    return bus.publishWithMsgId(
        {.subject = subject, .payload = body, .msgId = subject + "-" + body});
  };

  NatsBus first;
  REQUIRE(first.connect(options));
  REQUIRE(first.ensureStream({.name = stream,
                              .subjects = {subject},
                              .maxAgeNs = 60LL * 1000000000,
                              .duplicatesNs = 60LL * 1000000000}));

  Deliveries bound;
  const auto attached = first.subscribeDurable(collectingInto(feed, bound));
  REQUIRE(attached.has_value());
  REQUIRE(publish(first, "one"));
  CHECK(awaitPayloads(bound, 1) == std::vector<std::string>{"one"});
  CHECK(msgIdsOf(bound) == std::vector<std::string>{subject + "-one"});

  CHECK(first.unsubscribe(attached.value_or(0)));
  REQUIRE(publish(first, "two"));
  Deliveries rebound;
  REQUIRE(attachWithin(first, collectingInto(feed, rebound)).has_value());
  CHECK(awaitPayloads(rebound, 1) == std::vector<std::string>{"two"});
  first.drain();

  NatsBus second;
  REQUIRE(second.connect(options));
  REQUIRE(publish(second, "three"));
  Deliveries resumed;
  const auto resumedId = attachWithin(second, collectingInto(feed, resumed));
  REQUIRE(resumedId.has_value());
  CHECK(awaitPayloads(resumed, 1) == std::vector<std::string>{"three"});

  NatsBus rival;
  REQUIRE(rival.connect(options));
  Deliveries refused;
  CHECK_FALSE(rival.subscribeDurable(collectingInto(feed, refused)).has_value());
  REQUIRE(publish(second, "held"));
  CHECK(awaitPayloads(resumed, 2) ==
        std::vector<std::string>{"three", "held"});

  CHECK(second.unsubscribe(resumedId.value_or(0)));
  NatsBus::DurableInput replayAll = collectingInto(feed, refused);
  replayAll.deliverAll = true;
  CHECK_FALSE(rival.subscribeDurable(replayAll).has_value());

  Deliveries takeover;
  REQUIRE(attachWithin(rival, collectingInto(feed, takeover)).has_value());
  REQUIRE(publish(rival, "four"));
  CHECK(awaitPayloads(takeover, 1) == std::vector<std::string>{"four"});
  CHECK(awaitPayloads(refused, 1).empty());

  rival.drain();
  second.drain();
}

TEST_CASE("a durable that asks for the last message per subject starts at the newest one" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  NatsBus::Options options;
  options.url = broker;
  NatsBus bus;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 60LL * 1000000000,
                            .duplicatesNs = 60LL * 1000000000}));
  for (const char* body : {"a", "b", "c"})
    REQUIRE(bus.publishWithMsgId({.subject = subject, .payload = body, .msgId = subject + "-" + body}));

  const NatsBus::DurableInput lastOnly{.stream = stream,
                                       .durable = stream + "-last",
                                       .subject = subject,
                                       .deliverAll = false,
                                       .deliverLastPerSubject = true,
                                       .maxDeliver = 5,
                                       .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                       .handler = {}};
  Deliveries newest;
  REQUIRE(bus.subscribeDurable(collectingInto(lastOnly, newest)).has_value());
  CHECK(awaitPayloads(newest, 1) == std::vector<std::string>{"c"});
  REQUIRE(bus.publishWithMsgId({.subject = subject, .payload = "d", .msgId = subject + "-d"}));
  CHECK(awaitPayloads(newest, 2) == std::vector<std::string>{"c", "d"});

  const NatsBus::DurableInput everything{.stream = stream,
                                         .durable = stream + "-all",
                                         .subject = subject,
                                         .deliverAll = true,
                                         .deliverLastPerSubject = false,
                                         .maxDeliver = 5,
                                         .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                         .handler = {}};
  Deliveries replay;
  REQUIRE(bus.subscribeDurable(collectingInto(everything, replay)).has_value());
  CHECK(awaitPayloads(replay, 4) == std::vector<std::string>{"a", "b", "c", "d"});
  bus.drain();
}

TEST_CASE("a durable under another delivery policy is deleted and created again only for a feed that opts in" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  NatsBus::Options options;
  options.url = broker;
  NatsBus bus;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 60LL * 1000000000,
                            .duplicatesNs = 60LL * 1000000000}));
  const auto publish = [&](const char* body) {
    return bus.publishWithMsgId({.subject = subject, .payload = body, .msgId = subject + "-" + body});
  };
  for (const char* body : {"a", "b", "c"})
    REQUIRE(publish(body));

  const NatsBus::DurableInput previousBuild{.stream = stream,
                                            .durable = stream + "-feed",
                                            .subject = subject,
                                            .deliverAll = true,
                                            .deliverLastPerSubject = false,
                                            .recreateOnPolicyChange = false,
                                            .maxDeliver = 5,
                                            .maxAckPending = NatsBus::kOrderedMaxAckPending,
                                            .handler = {}};
  NatsBus::DurableInput conflicting = previousBuild;
  conflicting.deliverAll = false;
  conflicting.deliverLastPerSubject = true;
  NatsBus::DurableInput optedIn = conflicting;
  optedIn.recreateOnPolicyChange = true;

  Deliveries history;
  const auto first = bus.subscribeDurable(collectingInto(previousBuild, history));
  REQUIRE(first.has_value());
  CHECK(awaitPayloads(history, 3) == std::vector<std::string>{"a", "b", "c"});
  REQUIRE(bus.unsubscribe(first.value_or(0)));

  Deliveries refused;
  CHECK_FALSE(bus.subscribeDurable(collectingInto(conflicting, refused)).has_value());
  REQUIRE(publish("x"));
  Deliveries kept;
  const auto reattached = attachWithin(bus, collectingInto(previousBuild, kept));
  REQUIRE(reattached.has_value());
  CHECK(awaitPayloads(kept, 1).front() == "x");
  REQUIRE(bus.unsubscribe(reattached.value_or(0)));

  Deliveries recreated;
  const auto recreatedId = attachWithin(bus, collectingInto(optedIn, recreated));
  REQUIRE(recreatedId.has_value());
  CHECK(awaitPayloads(recreated, 1).front() == "x");
  REQUIRE(publish("y"));
  CHECK(awaitPayloads(recreated, 2) == std::vector<std::string>{"x", "y"});
  REQUIRE(bus.unsubscribe(recreatedId.value_or(0)));

  Deliveries untouched;
  const auto again = attachWithin(bus, collectingInto(optedIn, untouched));
  REQUIRE(again.has_value());
  REQUIRE(publish("z"));
  CHECK(awaitPayloads(untouched, 1).front() == "z");
  bus.drain();
}

TEST_CASE("an ordered durable redelivers a nak'd message before the next one" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();
  NatsBus::Options options;
  options.url = url;
  NatsBus bus;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 60LL * 1000000000,
                            .duplicatesNs = 60LL * 1000000000}));
  for (const char* body : {"a", "b", "c"})
    REQUIRE(bus.publishWithMsgId(
        {.subject = subject, .payload = body, .msgId = subject + "-" + body}));

  Deliveries seen;
  std::atomic<bool> refusedOnce{false};
  const auto started = std::chrono::steady_clock::now();
  std::atomic<int64_t> redeliveredAfterMs{0};
  const auto attached = bus.subscribeDurable(
      {.stream = stream,
       .durable = stream + "-ordered",
       .subject = subject,
       .deliverAll = true,
       .maxDeliver = 5,
       .maxAckPending = NatsBus::kOrderedMaxAckPending,
       .handler = [&](const NatsBus::DurableMessage& message,
                      const NatsBus::DurableSettlement& settlement) {
         const std::string payload(message.payload);
         {
           std::lock_guard lock(seen.mutex);
           seen.payloads.push_back(payload);
         }
         if (payload == "a" && !refusedOnce.exchange(true)) {
           settlement.nak();
           return;
         }
         if (payload == "a")
           redeliveredAfterMs = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
         settlement.ack();
       }});
  REQUIRE(attached.has_value());

  CHECK(awaitPayloads(seen, 4) ==
        std::vector<std::string>{"a", "a", "b", "c"});
  CHECK(redeliveredAfterMs.load() >= 900);

  bus.drain();
}

TEST_CASE("streamInfo reports a missing stream without a server roundtrip lie")
{
  NatsBus bus;
  CHECK_FALSE(bus.streamInfo("argus-test-stream-that-cannot-exist").has_value());
  bus.drain();
}

TEST_CASE("an in-progress mark on a settlement without a broker message is a no-op")
{
  int acks = 0;
  const NatsBus::DurableSettlement fake{.ack = [&acks] { ++acks; },
                                        .nak = [] {},
                                        .term = [] {}};
  fake.markInProgress();
  fake.ack();
  CHECK(acks == 1);

  int touches = 0;
  const NatsBus::DurableSettlement touched{.ack = [] {},
                                           .nak = [] {},
                                           .term = [] {},
                                           .inProgress = [&touches] { ++touches; }};
  touched.markInProgress();
  touched.markInProgress();
  CHECK(touches == 2);
}

TEST_CASE("a durable feed refuses a consumer its own stream does not carry")
{
  NatsBus bus;
  const NatsBus::StreamInput stream{.name = "argus-feed",
                                    .subjects = {"argus.feed.v1.event"},
                                    .maxAgeNs = 0,
                                    .duplicatesNs = 0};
  CHECK_FALSE(bus.subscribeDurableFeed(
                     {.stream = stream,
                      .consumer = {.stream = "argus-feed",
                                   .durable = "argus-feed-reader",
                                   .subject = "argus.other.v1.event",
                                   .deliverAll = false,
                                   .maxDeliver = 5,
                                   .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                   .handler = {}}})
                  .has_value());
  CHECK_FALSE(bus.subscribeDurableFeed(
                     {.stream = stream,
                      .consumer = {.stream = "another-stream",
                                   .durable = "argus-feed-reader",
                                   .subject = "argus.feed.v1.event",
                                   .deliverAll = false,
                                   .maxDeliver = 5,
                                   .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                   .handler = {}}})
                  .has_value());
  CHECK_FALSE(bus.subscribeDurableFeed(
                     {.stream = stream,
                      .consumer = {.stream = "argus-feed",
                                   .durable = "argus-feed-reader",
                                   .subject = "argus.feed.v1.event",
                                   .deliverAll = false,
                                   .maxDeliver = 5,
                                   .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                   .handler = {}}})
                  .has_value());
}

TEST_CASE("a durable feed turns core publishes into a backlog that survives the consumer" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();
  NatsBus::Options options;
  options.url = url;
  const std::string stream = isolatedStream();
  const std::string subject = isolatedSubject(stream);
  const NatsBus::DurableFeedInput feed{
      .stream = {.name = stream,
                 .subjects = {subject},
                 .maxAgeNs = 60LL * 1000000000,
                 .duplicatesNs = 0},
      .consumer = {.stream = stream,
                   .durable = stream + "-durable",
                   .subject = subject,
                   .deliverAll = false,
                   .maxDeliver = 5,
                   .maxAckPending = NatsBus::kDefaultMaxAckPending,
                   .handler = {}}};

  NatsBus bus;
  REQUIRE(bus.connect(options));
  Deliveries first;
  NatsBus::DurableFeedInput bound = feed;
  bound.consumer.handler = [&first](const NatsBus::DurableMessage& message,
                                    const NatsBus::DurableSettlement& settlement) {
    settlement.markInProgress();
    {
      std::scoped_lock lock(first.mutex);
      first.payloads.emplace_back(message.payload);
    }
    settlement.ack();
  };
  const auto id = bus.subscribeDurableFeed(bound);
  REQUIRE(id.has_value());
  REQUIRE(bus.publish(subject, "one"));
  CHECK(awaitPayloads(first, 1) == std::vector<std::string>{"one"});
  CHECK(bus.unsubscribe(id.value_or(0)));

  REQUIRE(bus.publish(subject, "while-away"));
  Deliveries resumed;
  NatsBus::DurableFeedInput again = feed;
  again.consumer = collectingInto(feed.consumer, resumed);
  std::optional<uint64_t> reattached;
  for (int attempt = 0; attempt < 40 && !reattached; ++attempt) {
    reattached = bus.subscribeDurableFeed(again);
    if (!reattached)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(reattached.has_value());
  CHECK(awaitPayloads(resumed, 1) == std::vector<std::string>{"while-away"});
  bus.drain();
}

namespace
{
struct CapturedLog
{
  std::mutex mutex;
  std::vector<std::string> lines;
};

CapturedLog& capturedLog()
{
  static CapturedLog captured;
  return captured;
}

class LogCapture
{
public:
  LogCapture()
  {
    trantor::Logger::setOutputFunction(
        [](const char* message, const uint64_t length) {
          {
            std::scoped_lock lock(capturedLog().mutex);
            capturedLog().lines.emplace_back(message, static_cast<size_t>(length));
          }
          std::cout.write(message, static_cast<std::streamsize>(length));
        },
        [] { std::cout << std::flush; });
  }

  ~LogCapture()
  {
    trantor::Logger::setOutputFunction(
        [](const char* message, const uint64_t length) {
          std::cout.write(message, static_cast<std::streamsize>(length));
        },
        [] { std::cout << std::flush; });
  }

  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
};

int logLinesMatching(std::string_view needle)
{
  std::scoped_lock lock(capturedLog().mutex);
  return static_cast<int>(std::ranges::count_if(
      capturedLog().lines, [needle](const std::string& line) {
        return line.find(needle) != std::string::npos;
      }));
}
}

TEST_CASE("a quiet durable stays silent while every failed attempt of its peer is logged" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);

  NatsBus bus;
  NatsBus::Options options;
  options.url = broker;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  {
    const LogCapture capture;
    const std::string missing = isolatedStream();
    CHECK_FALSE(bus
                    .subscribeDurable({.stream = missing,
                                       .durable = "quiet-durable",
                                       .subject = isolatedSubject(missing),
                                       .deliverAll = false,
                                       .quiet = true,
                                       .maxDeliver = 5,
                                       .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                       .handler = {}})
                    .has_value());
    CHECK(logLinesMatching("is not ready") == 0);

    CHECK_FALSE(bus
                    .subscribeDurable({.stream = missing,
                                       .durable = "loud-durable",
                                       .subject = isolatedSubject(missing),
                                       .deliverAll = false,
                                       .maxDeliver = 5,
                                       .maxAckPending = NatsBus::kDefaultMaxAckPending,
                                       .handler = {}})
                    .has_value());
    CHECK(logLinesMatching("is not ready") == 1);
  }

  const std::string ensured = isolatedStream();
  const std::string subject = isolatedSubject(ensured);
  const auto feed = bus.subscribeDurableFeed(
      {.stream = {.name = ensured,
                  .subjects = {subject},
                  .maxAgeNs = 60LL * 1000000000,
                  .duplicatesNs = 0},
       .consumer = {.stream = ensured,
                    .durable = "ensured-durable",
                    .subject = subject,
                    .deliverAll = false,
                    .maxDeliver = 5,
                    .maxAckPending = NatsBus::kDefaultMaxAckPending,
                    .handler = {}}});
  REQUIRE(feed.has_value());
  const auto created = bus.streamInfo(ensured);
  REQUIRE(created.has_value());
  CHECK(requireValue(created).subjects == std::vector<std::string>{subject});
  CHECK(bus.unsubscribe(requireValue(feed)));
  bus.drain();
}
