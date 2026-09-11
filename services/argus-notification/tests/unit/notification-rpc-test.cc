#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <argus/notification/v1/notification.grpc.pb.h>
#include <drogon/drogon.h>
#include <feature/rpc/notification-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <notification/notification-client.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kNotificationDb = "notification-rpc-test.db";

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

struct RecordedEmit
{
  int operation{0};
  std::string option;
  Json::Value body;
  std::vector<int64_t> users;
};

// Records the emits the notification service hands to the funnel.
class RecordingSink final : public UserChangeSink
{
public:
  void emitUser(int64_t userId, const SocketEmitDto& body) const override
  {
    emitUsers({userId}, body);
  }

  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override
  {
    std::lock_guard lock(mutex_);
    emits.push_back({static_cast<int>(body.operation),
                     tableNameToString(body.option), body.obj, userIds});
  }

  drogon::Task<void> publishAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedEmit> emits;
};

class RpcHarness
{
public:
  RpcHarness()
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
  return request;
}

struct WaitForEmitsInput
{
  const RecordingSink& sink;
  size_t expected{0};
  std::chrono::milliseconds timeout{};
};

bool waitForEmits(const WaitForEmitsInput& input)
{
  const auto deadline = std::chrono::steady_clock::now() + input.timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard lock(input.sink.mutex_);
      if (input.sink.emits.size() >= input.expected)
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard lock(input.sink.mutex_);
  return input.sink.emits.size() >= input.expected;
}
} // namespace

TEST_CASE("notification RPC creates fan-out rows and serves user pulls")
{
  std::remove(kNotificationDb);

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kNotificationDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  RecordingSink sink;
  user_change::setNotificationSink(&sink);

  RpcHarness harness;
  REQUIRE(harness.listening());
  NotificationClient sdk(harness.target());

  {
    // Create fan-out writes one row per user and emits Add.
    const auto created = sdk.createNotifications(createRequest(),
                                                identityFor(0));
    REQUIRE(created);
    CHECK(created->created() == 2);
    REQUIRE(waitForEmits({.sink = sink,
                         .expected = 2,
                         .timeout = std::chrono::seconds(5)}));

    {
      std::lock_guard lock(sink.mutex_);
      REQUIRE(sink.emits.size() == 2);
      for (const auto& emit : sink.emits) {
        CHECK(emit.operation == static_cast<int>(SyncOperation::Add));
        CHECK(emit.option == "notification");
        CHECK(emit.body["title"].asString() == "Front door: person");
        CHECK(emit.users.size() == 1);
      }
      CHECK(sink.emits[0].users[0] == 7);
      CHECK(sink.emits[1].users[0] == 8);
    }

    const auto rows = DbService::client()->execSqlSync(
        "SELECT user_id FROM notification ORDER BY user_id");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["user_id"].as<int64_t>() == 7);
    CHECK(rows[1]["user_id"].as<int64_t>() == 8);
  }

  {
    // Pull is user-scoped and exposes the last created row.
    argus::notification::v1::PullNotificationsRequest pull;
    pull.mutable_notification()->set_required_create(true);
    pull.mutable_notification()->set_find_last_created(true);

    const auto mine = sdk.pullNotifications(pull, identityFor(7));
    REQUIRE(mine);
    REQUIRE(mine->created_size() == 1);
    CHECK(mine->created(0).user_id() == 7);
    CHECK(mine->created(0).title() == "Front door: person");
    REQUIRE(mine->has_last_created());
    CHECK(mine->last_created().id() == mine->created(0).id());

    const auto others = sdk.pullNotifications(pull, identityFor(9));
    REQUIRE(others);
    CHECK(others->created_size() == 0);
    CHECK_FALSE(others->has_last_created());
  }

  {
    // Missing caller identity is unauthenticated.
    auto raw = argus::notification::v1::NotificationService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    grpc::ClientContext context;
    argus::notification::v1::CreateNotificationsResponse response;
    const grpc::Status status =
        raw->CreateNotifications(&context, createRequest(), &response);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }

  user_change::setNotificationSink(nullptr);
  drogon::app().quit();
  runner.join();
  std::remove(kNotificationDb);
}
