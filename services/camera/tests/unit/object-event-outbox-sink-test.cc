#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/operator/nats-object-event-sink.hxx>
#include <feature/operator/repositories/object-event-outbox/object-event-outbox-repository.hxx>
#include <sqlite/db-service.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace
{
constexpr const char* kSinkDb = "object-event-sink-test.db";

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

class Worker
{
public:
  explicit Worker(std::function<void()> body) : worker_(std::move(body)) {}

  Worker(std::function<void()> release, std::function<void()> body)
      : release_(std::move(release)), worker_(std::move(body))
  {
  }

  void join()
  {
    if (!worker_.joinable())
      return;
    if (release_)
      release_();
    worker_.join();
  }

  ~Worker() { join(); }

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

private:
  std::function<void()> release_;
  std::thread worker_;
};

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

bool waitForDrain(const NatsObjectEventSink& sink,
                  std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (sink.drained())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return sink.drained();
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
}

TEST_CASE("the sink health counters equal the durable outbox state")
{
  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kSinkDb, "default", -1});
  AppRunner runner;
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

    CHECK_FALSE(sink.drained());
    sink.requestStop();
    CHECK(waitForDrain(sink, std::chrono::seconds(5)));
    sink.requestStop();
    CHECK(sink.drained());
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

    const ObjectEventOutboxStats rowsBeforeBurst = outbox.stats();
    const auto started = std::chrono::steady_clock::now();
    Worker producer([&liveSink]() {
      for (int index = 0; index < 100; ++index)
        liveSink.publish(personEvent("flush:" + std::to_string(index)));
    });
    producer.join();
    CHECK(outbox.stats().pending == rowsBeforeBurst.pending + 100);

    liveSink.reconcile();
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (outbox.stats().pending == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const ObjectEventOutboxStats rowsAfterBurst = outbox.stats();
    CHECK(rowsAfterBurst.pending == 0);
    CHECK(rowsAfterBurst.pending + rowsAfterBurst.sent ==
          rowsBeforeBurst.pending + rowsBeforeBurst.sent + 100);
    CHECK(countersMatch(liveSink.health(), outbox.stats()));
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::seconds(3));

    {
      NatsObjectEventSink restarted(liveBus, liveConfig);
      restarted.reconcile();
      CHECK(countersMatch(restarted.health(), outbox.stats()));
    }
    liveBus->drain();
  }

  {
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
    Worker publisher([&gate]() { gate.arrive_and_wait(); },
                     [&]() { barrierSink.publish(personEvent(eventId)); });
    while (!hookReached.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int64_t at = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    CHECK(outbox.markSent(eventId, at));
    barrierSink.reconcile();
    publisher.join();
    CHECK(countersMatch(barrierSink.health(), outbox.stats()));
  }

  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
}
