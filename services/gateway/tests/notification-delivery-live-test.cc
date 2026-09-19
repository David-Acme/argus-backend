#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <sync/notification-delivery-consumer.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
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

int streamCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

std::string isolatedName(const std::string& prefix)
{
  return prefix + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(streamCounter());
}

// Unique database per execution, removed with its WAL/SHM on destruction.
class TempDb
{
public:
  TempDb() : path_(isolatedName("delivery-live-test") + ".db") {}

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

// Mutex-guarded dispatch record shared with NATS callback threads.
struct SharedDispatch
{
  void record(int64_t deliveryId)
  {
    {
      std::lock_guard lock(mutex);
      dispatched.push_back(deliveryId);
    }
    changed.notify_all();
  }

  bool waitFor(size_t expected, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, timeout, [&] {
      return dispatched.size() >= expected;
    });
  }

  size_t size()
  {
    std::lock_guard lock(mutex);
    return dispatched.size();
  }

  bool contains(int64_t deliveryId)
  {
    std::lock_guard lock(mutex);
    for (const auto value : dispatched) {
      if (value == deliveryId)
        return true;
    }
    return false;
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<int64_t> dispatched;
};

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

int64_t deliveryAttempts(int64_t deliveryId)
{
  const std::string value = scalar("SELECT attempts FROM "
                                   "notification_delivery_inbox WHERE "
                                   "delivery_id = " +
                                   std::to_string(deliveryId));
  return value.empty() ? -1 : std::stoll(value);
}

NotificationDeliveryEvent deliveryEvent(int64_t deliveryId)
{
  return {.deliveryId = deliveryId,
          .notificationId = 5000 + deliveryId,
          .userId = 7,
          .type = "camera",
          .title = "Front door",
          .body = "Person detected",
          .data = Json::Value(Json::objectValue),
          .createdAt = 1700000000};
}

struct PublishInput
{
  NatsBus& bus;
  std::string subject;
  int64_t deliveryId{0};
  std::string payload;
};

bool publishDelivery(const PublishInput& input)
{
  const std::string body =
      input.payload.empty()
          ? json_util::toString(deliveryEvent(input.deliveryId).toJson())
          : input.payload;
  return input.bus.publishWithMsgId(
      {.subject = input.subject,
       .payload = body,
       .msgId = notification_delivery::messageId(input.deliveryId)});
}

struct ConsumerInput
{
  NatsBus* bus{nullptr};
  SharedDispatch* shared{nullptr};
  std::string stream;
  std::string durable;
  std::string subject;
  int maxDeliver{10};
  int poisonMaxAttempts{3};
  bool poison{false};
};

NotificationDeliveryConsumer makeConsumer(const ConsumerInput& input)
{
  NotificationDeliveryConsumer::Dependencies dependencies;
  dependencies.bus = input.bus;
  if (input.poison) {
    dependencies.dispatch = [](const NotificationDeliveryEvent&) {
      throw std::runtime_error("poison dispatch");
    };
  }
  else {
    SharedDispatch* shared = input.shared;
    dependencies.dispatch = [shared](const NotificationDeliveryEvent& event) {
      shared->record(event.deliveryId);
    };
  }
  return NotificationDeliveryConsumer(
      std::move(dependencies),
      {.stream = input.stream,
       .durable = input.durable,
       .subject = input.subject,
       .maxDeliver = input.maxDeliver,
       .poisonMaxAttempts = input.poisonMaxAttempts});
}
} // namespace

TEST_CASE("delivery fan-out is at-least-once with inbox dedup")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; delivery live check skipped");
    return;
  }

  const TempDb db;
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA_PATH));

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedName("argus-test-delivery");
  const std::string subject = stream + ".events";
  const std::string durable = isolatedName("test-delivery");
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 120LL * 1000000000}));

  SharedDispatch first;
  auto consumer = makeConsumer({.bus = &bus,
                                .shared = &first,
                                .stream = stream,
                                .durable = durable,
                                .subject = subject,
                                .maxDeliver = 10,
                                .poisonMaxAttempts = 3,
                                .poison = false});

  REQUIRE(publishDelivery({.bus = bus,
                             .subject = subject,
                             .deliveryId = 1,
                             .payload = {}}));
  REQUIRE(publishDelivery({.bus = bus,
                           .subject = subject,
                           .deliveryId = 2,
                           .payload = {}}));
  REQUIRE(publishDelivery({.bus = bus,
                           .subject = subject,
                           .deliveryId = 1,
                           .payload = {}}));
  consumer.start();
  REQUIRE(first.waitFor(2, std::chrono::seconds(20)));
  CHECK(first.size() == 2);
  CHECK(first.contains(1));
  CHECK(first.contains(2));

  consumer.stop();
  REQUIRE(publishDelivery({.bus = bus,
                             .subject = subject,
                             .deliveryId = 3,
                             .payload = {}}));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(first.size() == 2);
  auto resumed = makeConsumer({.bus = &bus,
                               .shared = &first,
                               .stream = stream,
                               .durable = durable,
                               .subject = subject,
                               .maxDeliver = 10,
                               .poisonMaxAttempts = 3,
                               .poison = false});
  resumed.start();
  REQUIRE(first.waitFor(3, std::chrono::seconds(20)));
  CHECK(first.size() == 3);
  CHECK(first.contains(3));
  REQUIRE(drogon::sync_wait(resumed.handle(deliveryEvent(1))) ==
          DeliveryDisposition::Ack);
  CHECK(first.size() == 3);

  const std::string stream2 = isolatedName("argus-test-delivery");
  const std::string subject2 = stream2 + ".events";
  SharedDispatch second;
  auto late = makeConsumer({.bus = &bus,
                            .shared = &second,
                            .stream = stream2,
                            .durable = isolatedName("test-delivery"),
                            .subject = subject2,
                            .maxDeliver = 10,
                            .poisonMaxAttempts = 3,
                            .poison = false});
  late.start();
  REQUIRE(bus.ensureStream({.name = stream2,
                            .subjects = {subject2},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 120LL * 1000000000}));
  REQUIRE(publishDelivery({.bus = bus,
                             .subject = subject2,
                             .deliveryId = 4,
                             .payload = {}}));
  REQUIRE(second.waitFor(1, std::chrono::seconds(20)));
  CHECK(second.size() == 1);

  REQUIRE(publishDelivery({.bus = bus,
                           .subject = subject,
                           .deliveryId = 5,
                           .payload = "not-json"}));
  REQUIRE(publishDelivery({.bus = bus,
                           .subject = subject,
                           .deliveryId = 6,
                           .payload = "{\"deliveryId\":0}"}));
  REQUIRE(publishDelivery({.bus = bus,
                             .subject = subject,
                             .deliveryId = 7,
                             .payload = {}}));
  REQUIRE(first.waitFor(4, std::chrono::seconds(20)));
  CHECK(first.size() == 4);
  CHECK(first.contains(7));

  resumed.stop();
  late.stop();
  auto poison = makeConsumer({.bus = &bus,
                              .shared = nullptr,
                              .stream = stream,
                              .durable = isolatedName("test-delivery"),
                              .subject = subject,
                              .maxDeliver = 2,
                              .poisonMaxAttempts = 100,
                              .poison = true});
  poison.start();
  REQUIRE(publishDelivery({.bus = bus,
                             .subject = subject,
                             .deliveryId = 8,
                             .payload = {}}));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline &&
         deliveryAttempts(8) < 2)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(deliveryAttempts(8) == 2);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(deliveryAttempts(8) == 2);
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 8") == "received");
  poison.stop();

  bus.drain();
  drogon::app().quit();
  runner.join();
}
