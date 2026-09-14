#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/notification/v1/notification.grpc.pb.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/rpc/notification-rpc-service.hxx>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <notification/notification-client.hxx>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kGuardCredential = "guard-notif-cred";
constexpr const char* kGatewayCredential = "gateway-notif-cred";

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
  out << "[grpc]\ncaller_guard = \"" << kGuardCredential
      << "\"\ncaller_gateway = \"" << kGatewayCredential << "\"\n";
}

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

struct RecordedDelivery
{
  int64_t deliveryId{0};
  int64_t notificationId{0};
  int64_t userId{0};
  std::string title;
};

// Records the durable publishes the RPC service settles its intents with.
class RecordingDeliverySink final : public NotificationDeliverySink
{
public:
  bool ensureStream() const override { return true; }

  bool publish(const NotificationDeliveryEvent& event) const override
  {
    std::lock_guard lock(mutex_);
    published.push_back({.deliveryId = event.deliveryId,
                         .notificationId = event.notificationId,
                         .userId = event.userId,
                         .title = event.title});
    return true;
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedDelivery> published;
};

class RpcHarness
{
public:
  explicit RpcHarness(NotificationRpcService::Dependencies dependencies)
      : service_(std::move(dependencies))
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    target_ = "127.0.0.1:" + std::to_string(port);
  }

  ~RpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  bool listening() const { return server_ != nullptr; }
  const std::string& target() const { return target_; }

private:
  NotificationRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

argus::sdk::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "owner", .device = "test-device"};
}

argus::notification::v1::CreateNotificationsRequest createRequest()
{
  argus::notification::v1::CreateNotificationsRequest request;
  request.add_user_ids(7);
  request.add_user_ids(8);
  request.set_type("camera");
  request.set_title("Front door: person");
  request.set_body("Severity critical; detected person");
  request.set_data("{\"cameraId\":1}");
  request.set_command_id("cmd-1");
  return request;
}

struct WaitForPublishesInput
{
  const RecordingDeliverySink& sink;
  size_t expected{0};
  std::chrono::milliseconds timeout{};
};

bool waitForPublishes(const WaitForPublishesInput& input)
{
  const auto deadline = std::chrono::steady_clock::now() + input.timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard lock(input.sink.mutex_);
      if (input.sink.published.size() >= input.expected)
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard lock(input.sink.mutex_);
  return input.sink.published.size() >= input.expected;
}
} // namespace

TEST_CASE("notification RPC creates fan-out rows and serves user pulls")
{
  const TempDb db("notification-rpc-test");
  const TempFile config("notification-rpc-test", ".toml");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  auto deliverySink = std::make_shared<RecordingDeliverySink>();

  writeConfig(config.path());
  ConfigService::load(config.path());

  RpcHarness harness({.deliverySink = deliverySink,
                      .pushSink = {},
                      .pushRequired = false});
  REQUIRE(harness.listening());
  NotificationClient sdk(
      NotificationClientConfig{.target = harness.target(),
                               .credential = kGuardCredential});
  NotificationClient pullSdk(
      NotificationClientConfig{.target = harness.target(),
                               .credential = kGatewayCredential});

  {
    const auto created =
        sdk.createNotifications(createRequest(), identityFor(0));
    REQUIRE(created.outcome == NotificationRpcOutcome::Success);
    CHECK(created.created == 2);
    CHECK_FALSE(created.duplicate);
    REQUIRE(waitForPublishes({.sink = *deliverySink,
                              .expected = 2,
                              .timeout = std::chrono::seconds(5)}));

    {
      std::lock_guard lock(deliverySink->mutex_);
      REQUIRE(deliverySink->published.size() == 2);
      for (const auto& delivery : deliverySink->published) {
        CHECK(delivery.deliveryId > 0);
        CHECK(delivery.notificationId > 0);
        CHECK(delivery.title == "Front door: person");
      }
      CHECK(deliverySink->published[0].userId == 7);
      CHECK(deliverySink->published[1].userId == 8);
      CHECK(deliverySink->published[0].notificationId !=
            deliverySink->published[1].notificationId);
    }

    const auto rows = DbService::client()->execSqlSync(
        "SELECT user_id FROM notification ORDER BY user_id");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["user_id"].as<int64_t>() == 7);
    CHECK(rows[1]["user_id"].as<int64_t>() == 8);
  }

  {
    argus::notification::v1::PullNotificationsRequest pull;
    pull.mutable_notification()->set_required_create(true);
    pull.mutable_notification()->set_find_last_created(true);

    const auto mine = pullSdk.pullNotifications(pull, identityFor(7));
    REQUIRE(mine.outcome == NotificationRpcOutcome::Success);
    REQUIRE(mine.response.created_size() == 1);
    CHECK(mine.response.created(0).user_id() == 7);
    CHECK(mine.response.created(0).title() == "Front door: person");
    REQUIRE(mine.response.has_last_created());
    CHECK(mine.response.last_created().id() == mine.response.created(0).id());

    const auto others = pullSdk.pullNotifications(pull, identityFor(9));
    REQUIRE(others.outcome == NotificationRpcOutcome::Success);
    CHECK(others.response.created_size() == 0);
    CHECK_FALSE(others.response.has_last_created());
  }

  {
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    const auto tryCreate = [&raw](const std::string& credential) {
      grpc::ClientContext context;
      if (!credential.empty())
        argus::sdk::addCallerCredential(context, credential);
      argus::sdk::addCallerIdentity(context, identityFor(0));
      context.AddMetadata("x-argus-role", "owner");
      argus::notification::v1::CreateNotificationsResponse response;
      return raw->CreateNotifications(&context, createRequest(), &response)
          .error_code();
    };
    CHECK(tryCreate({}) == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(tryCreate("wrong") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(tryCreate("fleet-test") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(tryCreate(kGatewayCredential) == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(tryCreate(kGuardCredential) == grpc::StatusCode::OK);
  }

  {
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    grpc::ClientContext context;
    argus::sdk::addCallerCredential(context, kGuardCredential);
    argus::sdk::addCallerIdentity(context, identityFor(7));
    argus::notification::v1::PullNotificationsRequest pull;
    argus::notification::v1::PullNotificationsResponse response;
    CHECK(raw->PullNotifications(&context, pull, &response).error_code() ==
          grpc::StatusCode::UNAUTHENTICATED);
  }

  {
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    struct SendCreateInput
    {
      std::string commandId;
      std::vector<int64_t> users;
      std::string title;
      std::string body;
      std::string data;
    };
    const auto sendCreate = [&raw](const SendCreateInput& input) {
      grpc::ClientContext context;
      argus::sdk::addCallerCredential(context, kGuardCredential);
      argus::sdk::addCallerIdentity(context, identityFor(0));
      argus::notification::v1::CreateNotificationsRequest request;
      for (const int64_t user : input.users)
        request.add_user_ids(user);
      request.set_command_id(input.commandId);
      request.set_type("camera");
      request.set_title(input.title);
      request.set_body(input.body);
      request.set_data(input.data);
      argus::notification::v1::CreateNotificationsResponse response;
      return raw->CreateNotifications(&context, request, &response);
    };

    CHECK(sendCreate({.commandId = "cmd-fp-reorder",
                      .users = {7, 8},
                      .title = "t",
                      .body = "b",
                      .data = "{\"cameraId\":1,\"zone\":\"a\"}"})
              .ok());
    CHECK(sendCreate({.commandId = "cmd-fp-reorder",
                      .users = {8, 7},
                      .title = "t",
                      .body = "b",
                      .data = "{\"zone\":\"a\",\"cameraId\":1}"})
              .ok());

    CHECK(sendCreate({.commandId = "cmd-fp-dup",
                      .users = {7, 7, 8},
                      .title = "t",
                      .body = "b",
                      .data = "{}"})
              .ok());
    CHECK(sendCreate({.commandId = "cmd-fp-dup",
                      .users = {7, 8},
                      .title = "t",
                      .body = "b",
                      .data = "{}"})
              .ok());

    CHECK(sendCreate({.commandId = "cmd-fp-change",
                      .users = {7, 8},
                      .title = "t",
                      .body = "b",
                      .data = "{}"})
              .ok());
    CHECK(sendCreate({.commandId = "cmd-fp-change",
                      .users = {7, 9},
                      .title = "t",
                      .body = "b",
                      .data = "{}"})
              .error_code() == grpc::StatusCode::ALREADY_EXISTS);
    CHECK(sendCreate({.commandId = "cmd-fp-change",
                      .users = {7, 8},
                      .title = "other",
                      .body = "b",
                      .data = "{}"})
              .error_code() == grpc::StatusCode::ALREADY_EXISTS);
    CHECK(sendCreate({.commandId = "cmd-fp-change",
                      .users = {7, 8},
                      .title = "t",
                      .body = "b",
                      .data = "{\"x\":1}"})
              .error_code() == grpc::StatusCode::ALREADY_EXISTS);

    CHECK(sendCreate({.commandId = "cmd-fp-newline",
                      .users = {7},
                      .title = "a\\nb",
                      .body = "c",
                      .data = "{}"})
              .ok());
    CHECK(sendCreate({.commandId = "cmd-fp-newline",
                      .users = {7},
                      .title = "a",
                      .body = "b\\nc",
                      .data = "{}"})
              .error_code() == grpc::StatusCode::ALREADY_EXISTS);

    CHECK(sendCreate({.commandId = "cmd-bad-json",
                      .users = {7},
                      .title = "t",
                      .body = "b",
                      .data = "{not-json}"})
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  }

  {
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    struct SendRawInput
    {
      std::string commandId;
      std::vector<int64_t> users;
      std::string title;
    };
    const auto sendRaw = [&raw](const SendRawInput& input) {
      grpc::ClientContext context;
      argus::sdk::addCallerCredential(context, kGuardCredential);
      argus::sdk::addCallerIdentity(context, identityFor(0));
      argus::notification::v1::CreateNotificationsRequest request;
      for (const int64_t user : input.users)
        request.add_user_ids(user);
      request.set_command_id(input.commandId);
      request.set_type("camera");
      request.set_title(input.title);
      request.set_body("b");
      request.set_data("{}");
      argus::notification::v1::CreateNotificationsResponse response;
      return raw->CreateNotifications(&context, request, &response);
    };
    CHECK(sendRaw({.commandId = "cmd-bounds-empty", .users = {}, .title = "t"})
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(sendRaw({.commandId = "cmd-bounds-neg", .users = {-1}, .title = "t"})
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(sendRaw({.commandId = "cmd-bounds-title",
                   .users = {7},
                   .title = std::string(201, 'x')})
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    std::vector<int64_t> tooMany;
    for (int64_t index = 0; index < 1001; ++index)
      tooMany.push_back(index + 1);
    CHECK(sendRaw({.commandId = "cmd-bounds-many",
                   .users = tooMany,
                   .title = "t"})
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  }

  {
    grpc::ClientContext context;
    argus::sdk::addCallerCredential(context, kGuardCredential);
    argus::sdk::addCallerIdentity(context, identityFor(0));
    argus::notification::v1::CreateNotificationsRequest noCommand =
        createRequest();
    noCommand.clear_command_id();
    argus::notification::v1::CreateNotificationsResponse response;
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    CHECK(
        raw->CreateNotifications(&context, noCommand, &response).error_code() ==
        grpc::StatusCode::INVALID_ARGUMENT);
  }

  drogon::app().quit();
  runner.join();
}
