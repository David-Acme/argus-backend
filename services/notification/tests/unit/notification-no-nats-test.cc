#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <drogon/drogon.h>
#include <feature/rpc/notification-rpc-service.hxx>
#include <fstream>
#include <optional>
#include <notification/notification-delivery-sink.hxx>
#include <config/config-service.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

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

class TempFile
{
public:
  TempFile(const char* stem, const char* extension)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + extension)
  {
  }

  ~TempFile() { std::remove(path_.c_str()); }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

void writeConfig(const std::string& path)
{
  std::ofstream out(path, std::ios::trunc);
  out << "[grpc]\ncaller_guard = \"no-nats-guard\"\n"
         "caller_gateway = \"no-nats-gateway\"\n";
}

// Runs the app and stops it however the owning scope leaves it. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
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

int64_t deliveryCount(const std::string& status)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM notification_delivery WHERE status = '" +
      status + "'");
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

NotificationBatchInput makeBatch(const std::string& commandId)
{
  NotificationBatchInput batch;
  batch.userIds = {11};
  batch.notification.type = "camera";
  batch.notification.title = "Front door";
  batch.notification.body = "Person detected";
  batch.commandId = commandId;
  return batch;
}

class GatedDeliverySink final : public NotificationDeliverySink
{
public:
  bool ensureStream() const override { return streamOk; }

  bool publish(const NotificationDeliveryEvent&) const override
  {
    ++publishes;
    return armed;
  }

  mutable bool streamOk{true};
  mutable bool armed{false};
  mutable int publishes{0};
};

int64_t maxDeliveryId()
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COALESCE(MAX(id), 0) AS top FROM notification_delivery");
  if (rows.empty())
    return -1;
  return rows.front()["top"].as<int64_t>();
}

struct SharedBoot
{
  TempDb db{"notification-no-nats-test"};
  TempFile config{"notification-no-nats-test", ".toml"};
  // The app's thread is held by an owner that stops it on destruction, so the
  // throws below unwind into a safe teardown: quit and join a live loop, or
  // detach a boot that never reached one, instead of destroying a joinable
  // std::thread and terminating.
  std::optional<AppRunner> runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
    // The app must be configured before it runs, so the owner is emplaced here
    // rather than in a member initialiser.
    runner.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA))
      throw std::runtime_error("notification schema apply failed");
    writeConfig(config.path());
    ConfigService::load(config.path());
  }

  ~SharedBoot()
  {
    // The owner stops the app; releasing it in the body keeps that stop ahead
    // of the database and config files the TempDb and TempFile members remove
    // after this body.
    runner.reset();
  }
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}
} // namespace

TEST_CASE("no NATS configured keeps intents pending without terminating")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  const NotificationService unsinked;
  CHECK_FALSE(unsinked.hasDeliverySink());

  NotificationRpcService rpc(NotificationRpcService::Dependencies{});
  rpc.startDeliveryReconciler();
  const auto created =
      drogon::sync_wait(unsinked.createManyAndEmit(makeBatch("no-nats-1")));
  CHECK_FALSE(created.duplicate);
  CHECK(created.createdCount == 1);
  REQUIRE(deliveryCount("pending") == 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(deliveryCount("pending") == 1);
  CHECK(deliveryCount("sent") == 0);

  drogon::sync_wait(unsinked.createManyAndEmit(makeBatch("no-nats-2")));
  REQUIRE(deliveryCount("pending") == 2);
  CHECK(drogon::sync_wait(unsinked.deliverPending()) ==
        DeliverPendingOutcome::NoSinkInstalled);
  CHECK(deliveryCount("pending") == 2);
  CHECK(drogon::sync_wait(unsinked.pendingBacklog()) == 2);

  auto sink = std::make_shared<GatedDeliverySink>();
  sink->streamOk = false;
  const NotificationService unstreamed({.deliverySink = sink,
                                        .pushSink = {},
                                        .pushRequired = false});
  CHECK(unstreamed.hasDeliverySink());
  CHECK(drogon::sync_wait(unstreamed.deliverPending()) ==
        DeliverPendingOutcome::StreamUnavailable);
  CHECK(deliveryCount("pending") == 2);
  CHECK(sink->publishes == 0);

  sink->streamOk = true;
  CHECK(drogon::sync_wait(unstreamed.deliverPending()) ==
        DeliverPendingOutcome::PublishRefused);
  CHECK(deliveryCount("pending") == 2);
  CHECK(deliveryCount("sent") == 0);

  sink->armed = true;
  CHECK(drogon::sync_wait(unstreamed.deliverPending()) ==
        DeliverPendingOutcome::Settled);
  CHECK(deliveryCount("pending") == 0);
  CHECK(deliveryCount("sent") == 2);

  auto pushless = std::make_shared<GatedDeliverySink>();
  pushless->armed = true;
  const NotificationService pushRequired({.deliverySink = pushless,
                                          .pushSink = {},
                                          .pushRequired = true});
  drogon::sync_wait(pushRequired.createManyAndEmit(makeBatch("no-nats-3")));
  CHECK(deliveryCount("pending") == 0);
  CHECK(deliveryCount("sent") == 3);
}

TEST_CASE("the reconciler names the pending backlog when the stream is down")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  const int64_t floor = maxDeliveryId();
  const int64_t pendingBefore = deliveryCount("pending");

  auto sink = std::make_shared<GatedDeliverySink>();
  sink->streamOk = false;
  NotificationRpcService rpc(
      NotificationRpcService::Dependencies{.deliverySink = sink,
                                           .pushSink = {},
                                           .pushRequired = false});
  rpc.startDeliveryReconciler();
  const NotificationService service({.deliverySink = sink,
                                     .pushSink = {},
                                     .pushRequired = false});
  drogon::sync_wait(service.createManyAndEmit(makeBatch("nbk:1")));

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(deliveryCount("pending") == pendingBefore + 1);
  CHECK(sink->publishes == 0);

  DbService::client()->execSqlSync(
      "DELETE FROM notification_delivery WHERE id > " +
      std::to_string(floor));
}
