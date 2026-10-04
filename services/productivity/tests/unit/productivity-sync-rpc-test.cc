#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <config/config-service.hxx>
#include <doctest/doctest.h>

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <drogon/drogon.h>
#include <feature/sync/productivity-sync-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <productivity/productivity-sync-client.hxx>
#include <sqlite/db-service.hxx>

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
constexpr const char* kSyncCredential = "productivity-sync-rpc-test-credential";

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

argus::client::CallerIdentity identityFor(int64_t userId)
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
}

TEST_CASE("productivity sync RPC scopes pulls by caller and serves tombstones")
{
  std::remove(kProductivityDb);
  std::remove((std::string(kProductivityDb) + "-wal").c_str());
  std::remove((std::string(kProductivityDb) + "-shm").c_str());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kProductivityDb, "default", -1});

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  auto client = DbService::productivityClient();
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

  ConfigService::setRuntimeString("grpc.caller_sync", kSyncCredential);
  RpcHarness harness;
  REQUIRE(harness.listening());
  ProductivitySyncClient sdk(
      {.target = harness.target(), .credential = kSyncCredential});

  {
    ProductivitySyncClient stranger(
        {.target = harness.target(), .credential = "not-the-sync-secret"});
    CHECK_FALSE(stranger.pullTable(projectPull(), identityFor(42)));
    ProductivitySyncClient anonymous(
        {.target = harness.target(), .credential = ""});
    CHECK_FALSE(anonymous.pullTable(projectPull(), identityFor(42)));
  }

  {
    const auto owner = sdk.pullTable(projectPull(), identityFor(42));
    if (!owner) {
      FAIL("expected a value in owner");
      return;
    }
    REQUIRE(owner->has_project());
    REQUIRE(owner->project().created_size() == 2);
    CHECK(owner->project().created(0).id() == 1);
    CHECK(owner->project().created(1).id() == 2);

    const auto other = sdk.pullTable(projectPull(), identityFor(7));
    if (!other) {
      FAIL("expected a value in other");
      return;
    }
    REQUIRE(other->project().created_size() == 1);
    CHECK(other->project().created(0).id() == 2);
  }

  {
    const auto rows = sdk.pullTable(reminderPull(), identityFor(7));
    if (!rows) {
      FAIL("expected a value in rows");
      return;
    }
    REQUIRE(rows->has_reminder());
    REQUIRE(rows->reminder().created_size() == 1);
    CHECK(rows->reminder().created(0).target_user_id() == 7);
  }

  {
    const auto created = sdk.pullTable(taskPull(false), identityFor(42));
    if (!created) {
      FAIL("expected a value in created");
      return;
    }
    REQUIRE(created->project_task().created_size() == 1);
    CHECK(created->project_task().created(0).id() == 2);
    CHECK(created->project_task().created(0).sort_order() == doctest::Approx(2.0));

    const auto deleted = sdk.pullTable(taskPull(true), identityFor(42));
    if (!deleted) {
      FAIL("expected a value in deleted");
      return;
    }
    REQUIRE(deleted->project_task().deleted_size() == 1);
    CHECK(deleted->project_task().deleted(0).id() == 1);
    CHECK(deleted->project_task().deleted(0).deleted_at() == 1500);

    argus::productivity::v1::PullTableRequest last;
    last.mutable_project()->set_find_last_created(true);
    const auto lastRow = sdk.pullTable(last, identityFor(42));
    if (!lastRow) {
      FAIL("expected a value in lastRow");
      return;
    }
    REQUIRE(lastRow->project().has_last_created());
    CHECK(lastRow->project().last_created().id() == 2);

    argus::productivity::v1::PullTableRequest lastDeleted;
    lastDeleted.mutable_project_task()->set_find_last_deleted(true);
    const auto tombstone = sdk.pullTable(lastDeleted, identityFor(42));
    if (!tombstone) {
      FAIL("expected a value in tombstone");
      return;
    }
    REQUIRE(tombstone->project_task().has_last_deleted());
    CHECK(tombstone->project_task().last_deleted().id() == 1);
  }

  {
    client->execSqlSync(
        "INSERT INTO project_task (id, project_id, created_by, title, status, "
        "priority, sort_order, created_at) "
        "VALUES (3, 1, 42, 'Oil hinge', 'todo', 'low', 3.0, 1001)");
    client->execSqlSync(
        "INSERT INTO project_member (id, project_id, user_id, access, created_at) "
        "VALUES (2, 1, 9, 'view', 5000)");

    argus::productivity::v1::PullTableRequest caughtUp = taskPull(false);
    caughtUp.mutable_project_task()->mutable_created()->set_start_time(4000);
    const auto missed = sdk.pullTable(caughtUp, identityFor(9));
    if (!missed) {
      FAIL("expected a value in missed");
      return;
    }
    CHECK(missed->project_task().created_size() == 0);

    argus::productivity::v1::PullTableRequest grants;
    grants.mutable_project_member()->set_required_create(true);
    grants.mutable_project_member()->mutable_created()->set_start_time(4000);
    const auto grant = sdk.pullTable(grants, identityFor(9));
    if (!grant) {
      FAIL("expected a value in grant");
      return;
    }
    REQUIRE(grant->project_member().created_size() == 1);
    CHECK(grant->project_member().created(0).project_id() == 1);

    argus::productivity::v1::PullTableRequest scopedTasks = taskPull(false);
    scopedTasks.mutable_project_task()->add_scope_ids(1);
    const auto tasks = sdk.pullTable(scopedTasks, identityFor(9));
    if (!tasks) {
      FAIL("expected a value in tasks");
      return;
    }
    REQUIRE(tasks->project_task().created_size() == 2);
    CHECK(tasks->project_task().created(0).id() == 2);
    CHECK(tasks->project_task().created(1).id() == 3);

    argus::productivity::v1::PullTableRequest nextPage = scopedTasks;
    nextPage.mutable_project_task()->mutable_created()->set_start_time(1001);
    nextPage.mutable_project_task()->mutable_created()->set_start_id(2);
    const auto rest = sdk.pullTable(nextPage, identityFor(9));
    if (!rest) {
      FAIL("expected a value in rest");
      return;
    }
    REQUIRE(rest->project_task().created_size() == 1);
    CHECK(rest->project_task().created(0).id() == 3);

    argus::productivity::v1::PullTableRequest scopedProject = projectPull();
    scopedProject.mutable_project()->add_scope_ids(1);
    scopedProject.mutable_project()->add_scope_ids(2);
    const auto projects = sdk.pullTable(scopedProject, identityFor(9));
    if (!projects) {
      FAIL("expected a value in projects");
      return;
    }
    REQUIRE(projects->project().created_size() == 1);
    CHECK(projects->project().created(0).id() == 1);

    argus::productivity::v1::PullTableRequest foreign = taskPull(false);
    foreign.mutable_project_task()->add_scope_ids(1);
    const auto outsider = sdk.pullTable(foreign, identityFor(8));
    if (!outsider) {
      FAIL("expected a value in outsider");
      return;
    }
    CHECK(outsider->project_task().created_size() == 0);

    argus::productivity::v1::PullTableRequest scopedEvent;
    scopedEvent.mutable_calendar_event()->set_required_create(true);
    scopedEvent.mutable_calendar_event()->add_scope_ids(1);
    const auto event = sdk.pullTable(scopedEvent, identityFor(7));
    if (!event) {
      FAIL("expected a value in event");
      return;
    }
    REQUIRE(event->calendar_event().created_size() == 1);
    CHECK(event->calendar_event().created(0).id() == 1);

    client->execSqlSync(
        "UPDATE project_member SET deleted_at = 6000 WHERE id = 2");
    argus::productivity::v1::PullTableRequest revocations;
    revocations.mutable_project_member()->set_required_deleted(true);
    revocations.mutable_project_member()->mutable_deleted()->set_start_time(5500);
    const auto revoked = sdk.pullTable(revocations, identityFor(9));
    if (!revoked) {
      FAIL("expected a value in revoked");
      return;
    }
    REQUIRE(revoked->project_member().deleted_size() == 1);
    CHECK(revoked->project_member().deleted(0).id() == 2);
    const auto after = sdk.pullTable(scopedTasks, identityFor(9));
    if (!after) {
      FAIL("expected a value in after");
      return;
    }
    CHECK(after->project_task().created_size() == 0);

    argus::productivity::v1::PullTableRequest unscopable = reminderPull();
    unscopable.mutable_reminder()->add_scope_ids(1);
    CHECK_FALSE(sdk.pullTable(unscopable, identityFor(7)));

    argus::productivity::v1::PullTableRequest oversized = taskPull(false);
    for (int64_t id = 1; id <= 51; ++id)
      oversized.mutable_project_task()->add_scope_ids(id);
    CHECK_FALSE(sdk.pullTable(oversized, identityFor(9)));

    argus::productivity::v1::PullTableRequest negative = taskPull(false);
    negative.mutable_project_task()->add_scope_ids(-1);
    CHECK_FALSE(sdk.pullTable(negative, identityFor(9)));
  }

  {
    auto raw = argus::productivity::v1::SyncService::NewStub(
        argus::client::makeChannel(harness.target()));
    grpc::ClientContext context;
    argus::productivity::v1::PullTableResponse response;
    const grpc::Status status = raw->PullTable(&context, projectPull(), &response);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }

  std::remove(kProductivityDb);
  std::remove((std::string(kProductivityDb) + "-wal").c_str());
  std::remove((std::string(kProductivityDb) + "-shm").c_str());
}
