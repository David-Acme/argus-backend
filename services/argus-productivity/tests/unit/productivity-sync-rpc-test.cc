#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <drogon/drogon.h>
#include <feature/sync/productivity-sync-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <productivity/productivity-sync-client.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kProductivityDb = "productivity-sync-rpc-test.db";

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
  ProductivitySyncRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

argus::sdk::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "owner", .device = "test-device"};
}

argus::productivity::v1::PullTableRequest projectPull()
{
  argus::productivity::v1::PullTableRequest request;
  request.mutable_project()->set_required_create(true);
  return request;
}

argus::productivity::v1::PullTableRequest reminderPull()
{
  argus::productivity::v1::PullTableRequest request;
  request.mutable_reminder()->set_required_create(true);
  return request;
}

argus::productivity::v1::PullTableRequest taskPull(bool deleted)
{
  argus::productivity::v1::PullTableRequest request;
  auto* body = request.mutable_project_task();
  if (deleted)
    body->set_required_deleted(true);
  else
    body->set_required_create(true);
  return request;
}
} // namespace

TEST_CASE("productivity sync RPC scopes pulls by caller and serves tombstones")
{
  std::remove(kProductivityDb);

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kProductivityDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  auto client = DbService::client();
  client->execSqlSync(
      "INSERT INTO project (id, owner_id, name, status, color, created_at) "
      "VALUES (1, 42, 'Home', 'active', '', 1000)");
  client->execSqlSync(
      "INSERT INTO project (id, owner_id, name, status, color, created_at) "
      "VALUES (2, 7, 'Work', 'active', '', 1001)");
  client->execSqlSync(
      "INSERT INTO project_member (id, project_id, user_id, access, created_at) "
      "VALUES (1, 2, 42, 'view', 1002)");
  client->execSqlSync(
      "INSERT INTO reminder (id, created_by, target_user_id, title, "
      "scheduled_at, created_at) VALUES (1, 42, 7, 'Pill', 1000, 1000)");
  client->execSqlSync(
      "INSERT INTO reminder_detail (id, reminder_id, created_by, content, "
      "status, file_paths, created_at) "
      "VALUES (1, 1, 42, 'note', 'pending', '', 1000)");
  client->execSqlSync(
      "INSERT INTO project_task (id, project_id, created_by, assignee_id, "
      "title, status, priority, sort_order, created_at, deleted_at) "
      "VALUES (1, 1, 42, 42, 'Fix door', 'todo', 'high', 1.5, 1000, 1500)");
  client->execSqlSync(
      "INSERT INTO project_task (id, project_id, created_by, assignee_id, "
      "title, status, priority, sort_order, created_at) "
      "VALUES (2, 1, 42, 42, 'Paint', 'doing', 'none', 2.0, 1001)");
  client->execSqlSync(
      "INSERT INTO calendar_event (id, created_by, owner_id, title, starts_at, "
      "is_all_day, created_at) VALUES (1, 42, 42, 'Dentist', 2000, 0, 1000)");
  client->execSqlSync(
      "INSERT INTO calendar_event_share (id, calendar_event_id, user_id, "
      "access, created_at) VALUES (1, 1, 7, 'view', 1000)");

  RpcHarness harness;
  REQUIRE(harness.listening());
  ProductivitySyncClient sdk(harness.target());

  {
    // Personal tables scope by owner and membership.
    const auto owner = sdk.pullTable(projectPull(), identityFor(42));
    REQUIRE(owner);
    REQUIRE(owner->has_project());
    REQUIRE(owner->project().created_size() == 2);
    CHECK(owner->project().created(0).id() == 1);
    CHECK(owner->project().created(1).id() == 2);

    const auto other = sdk.pullTable(projectPull(), identityFor(7));
    REQUIRE(other);
    REQUIRE(other->project().created_size() == 1);
    CHECK(other->project().created(0).id() == 2);
  }

  {
    // Reminder tables are not user-scoped.
    const auto rows = sdk.pullTable(reminderPull(), identityFor(7));
    REQUIRE(rows);
    REQUIRE(rows->has_reminder());
    REQUIRE(rows->reminder().created_size() == 1);
    CHECK(rows->reminder().created(0).target_user_id() == 7);
  }

  {
    // Tombstones and last rows round-trip.
    const auto created = sdk.pullTable(taskPull(false), identityFor(42));
    REQUIRE(created);
    REQUIRE(created->project_task().created_size() == 1);
    CHECK(created->project_task().created(0).id() == 2);
    CHECK(created->project_task().created(0).sort_order() == doctest::Approx(2.0));

    const auto deleted = sdk.pullTable(taskPull(true), identityFor(42));
    REQUIRE(deleted);
    REQUIRE(deleted->project_task().deleted_size() == 1);
    CHECK(deleted->project_task().deleted(0).id() == 1);
    CHECK(deleted->project_task().deleted(0).deleted_at() == 1500);

    argus::productivity::v1::PullTableRequest last;
    last.mutable_project()->set_find_last_created(true);
    const auto lastRow = sdk.pullTable(last, identityFor(42));
    REQUIRE(lastRow);
    REQUIRE(lastRow->project().has_last_created());
    CHECK(lastRow->project().last_created().id() == 2);

    argus::productivity::v1::PullTableRequest lastDeleted;
    lastDeleted.mutable_project_task()->set_find_last_deleted(true);
    const auto tombstone = sdk.pullTable(lastDeleted, identityFor(42));
    REQUIRE(tombstone);
    REQUIRE(tombstone->project_task().has_last_deleted());
    CHECK(tombstone->project_task().last_deleted().id() == 1);
  }

  {
    // Missing caller identity is unauthenticated.
    auto raw = argus::productivity::v1::SyncService::NewStub(
        argus::sdk::makeChannel(harness.target()));
    grpc::ClientContext context;
    argus::productivity::v1::PullTableResponse response;
    const grpc::Status status = raw->PullTable(&context, projectPull(), &response);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }

  drogon::app().quit();
  runner.join();
  std::remove(kProductivityDb);
}
