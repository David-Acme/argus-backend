#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <operator/nats-object-event-sink.hxx>
#include <shared/repositories/object-event-outbox/object-event-outbox-repository.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace
{
constexpr const char* kSinkDb = "object-event-sink-test.db";

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

bool countersMatch(const Json::Value& health,
                   const ObjectEventOutboxStats& stats)
{
  return health["pending"].as<int64_t>() == stats.pending &&
         health["sent"].as<int64_t>() == stats.sent &&
         health["overflowDropped"].as<int64_t>() == stats.overflowDropped;
}

ObjectDetectedEvent personEvent(const std::string& eventId)
{
  ObjectDetectedEvent event;
  event.eventId = eventId;
  event.cameraId = 1;
  event.cameraName = "front";
  event.rule = "person_day";
  event.severity = "info";
  event.trackId = 1;
  DetectedEventObject object;
  object.name = "person";
  object.trackId = 1;
  event.objects.push_back(object);
  return event;
}
} // namespace

TEST_CASE("the sink health counters equal the durable outbox state")
{
  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kSinkDb, "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));

  auto bus = std::make_shared<NatsBus>();
  ObjectEventOutboxRepository outbox;
  NatsObjectEventSink::Config config;
  config.cooldownMs = 0;
  config.maxPending = 2;
  config.retryMs = 50;
  config.sessionTag = "sink-test";

  {
    NatsObjectEventSink sink(bus, config);
    sink.reconcile();

    CHECK(sink.publish(personEvent("sink:1")) ==
          ObjectEventPublishResult::Recorded);
    CHECK(countersMatch(sink.health(), outbox.stats()));

    CHECK(sink.publish(personEvent("sink:1")) ==
          ObjectEventPublishResult::Duplicate);
    CHECK(outbox.stats().pending == 1);
    CHECK(countersMatch(sink.health(), outbox.stats()));

    CHECK(sink.publish(personEvent("sink:2")) ==
          ObjectEventPublishResult::Recorded);
    CHECK(outbox.stats().pending == 2);
    CHECK(countersMatch(sink.health(), outbox.stats()));

    CHECK(sink.publish(personEvent("sink:3")) ==
          ObjectEventPublishResult::Recorded);
    CHECK(outbox.stats().overflowDropped == 1);
    CHECK(countersMatch(sink.health(), outbox.stats()));
  }

  {
    NatsObjectEventSink restarted(bus, config);
    restarted.reconcile();
    CHECK(countersMatch(restarted.health(), outbox.stats()));
  }

  {
    NatsObjectEventSink::Config bootConfig;
    bootConfig.cooldownMs = 60000;
    bootConfig.maxPending = 100;
    bootConfig.retryMs = 50;
    {
      NatsObjectEventSink firstBoot(bus, bootConfig);
      firstBoot.reconcile();
      CHECK(firstBoot.publish(personEvent("boot1:1")) ==
            ObjectEventPublishResult::Recorded);
    }
    NatsObjectEventSink secondBoot(bus, bootConfig);
    secondBoot.reconcile();
    CHECK(secondBoot.publish(personEvent("boot2:1")) ==
          ObjectEventPublishResult::Recorded);
    CHECK(secondBoot.publish(personEvent("boot2:2")) ==
          ObjectEventPublishResult::Suppressed);
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    auto liveBus = std::make_shared<NatsBus>();
    NatsBus::Options options;
    options.url = url;
    options.reconnectWaitMs = 200;
    options.maxReconnects = 5;
    REQUIRE(liveBus->connect(options));
    NatsObjectEventSink::Config liveConfig;
    liveConfig.cooldownMs = 0;
    liveConfig.maxPending = 1000;
    liveConfig.retryMs = 20;
    liveConfig.sessionTag = "flush-test";
    liveConfig.streamName = "ARGUS_SINK_TEST";
    liveConfig.publishSubject = "argus.test.sink.flush";
    NatsObjectEventSink liveSink(liveBus, liveConfig);
    liveSink.reconcile();

    std::thread producer([&liveSink]() {
      for (int index = 0; index < 40; ++index)
        liveSink.publish(personEvent("flush:" + std::to_string(index)));
    });
    producer.join();

    for (int attempt = 0; attempt < 200; ++attempt) {
      if (outbox.stats().pending == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(outbox.stats().pending == 0);
    CHECK(countersMatch(liveSink.health(), outbox.stats()));

    {
      NatsObjectEventSink restarted(liveBus, liveConfig);
      restarted.reconcile();
      CHECK(countersMatch(restarted.health(), outbox.stats()));
    }
    liveBus->drain();
  }

  {
    // A committed enqueue and a concurrent flush must converge on the DB.
    NatsObjectEventSink barrierSink(bus, config);
    barrierSink.reconcile();
    std::barrier gate(2);
    std::atomic<bool> hookReached{false};
    barrierSink.syncHook = [&](const std::string& point) {
      if (point == "enqueue_post_commit") {
        hookReached.store(true);
        gate.arrive_and_wait();
      }
    };
    const std::string eventId = "race:1";
    std::thread publisher(
        [&]() { barrierSink.publish(personEvent(eventId)); });
    while (!hookReached.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int64_t at = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    CHECK(outbox.markSent(eventId, at));
    barrierSink.reconcile();
    gate.arrive_and_wait();
    publisher.join();
    CHECK(countersMatch(barrierSink.health(), outbox.stats()));
  }

  drogon::app().quit();
  runner.join();
  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
}
