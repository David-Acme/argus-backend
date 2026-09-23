#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/nats-camera-change-sink.hxx>
#include <drogon/drogon.h>
#include <feature/api/camera/services/camera-feature-service.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{
constexpr const char* kTransactionDb = "camera-change-transaction-test.db";

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

class RefusingSink : public CameraChangeSink
{
public:
  explicit RefusingSink(const drogon::orm::DbClient* pooled) : pooled_(pooled)
  {
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the emit");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAudit(const ModuleAuditInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the audit");
    co_return;
  }

  void refuse(bool value) const { refuse_ = value; }
  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool sawClient() const { return sawClient_; }
  [[nodiscard]] bool transactional() const { return transactional_; }

private:
  void note(const drogon::orm::DbClient* client) const
  {
    ++calls_;
    sawClient_ = client != nullptr;
    transactional_ = sawClient_ && client != pooled_;
  }

  mutable bool refuse_{false};
  mutable int calls_{0};
  mutable bool sawClient_{false};
  mutable bool transactional_{false};
  const drogon::orm::DbClient* pooled_;
};

CreateCameraDto cameraBody(const std::string& name)
{
  CreateCameraDto dto;
  dto.name = name;
  dto.manufacturer = "tapo";
  dto.model = "c200";
  dto.ip = "127.0.0.1";
  dto.port = 554;
  dto.username = "patio";
  dto.password = "secret";
  dto.driver = "tapo";
  dto.icon = "camera";
  dto.recordMode = "continuous";
  dto.retentionDays = 7;
  return dto;
}

int64_t liveCameras(const std::string& name)
{
  const auto rows = DbService::cameraClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM camera WHERE name = ? "
      "AND deleted_at IS NULL",
      name);
  if (rows.empty())
    return -1;
  return rows.front()["total"].as<int64_t>();
}

std::string cameraName(int64_t id)
{
  const auto rows = DbService::cameraClient()->execSqlSync(
      "SELECT name FROM camera WHERE id = ?", id);
  if (rows.empty())
    return {};
  return rows.front()["name"].as<std::string>();
}
}

TEST_CASE("a camera write and its change are one unit of work")
{
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kTransactionDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));

  CameraFeatureService cameras;
  const auto pooled = DbService::cameraClient();

  RefusingSink sink(pooled.get());
  camera_change::setSink(&sink);

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(cameras.create(cameraBody("Refused Cam"))),
      std::runtime_error);
  CHECK(liveCameras("Refused Cam") == 0);

  sink.refuse(false);
  const int beforeCreate = sink.calls();
  const auto created = drogon::sync_wait(cameras.create(cameraBody("Kept Cam")));
  CHECK(created.id > 0);
  CHECK(sink.calls() == beforeCreate + 1);
  CHECK(sink.sawClient());
  CHECK(sink.transactional());
  CHECK(liveCameras("Kept Cam") == 1);

  UpdateCameraDto rename;
  rename.name = "Renamed Cam";

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(cameras.update(created.id, rename)),
                  std::runtime_error);
  CHECK(cameraName(created.id) == "Kept Cam");

  sink.refuse(false);
  CHECK(drogon::sync_wait(cameras.update(created.id, rename)).has_value());
  CHECK(cameraName(created.id) == "Renamed Cam");
  CHECK(sink.transactional());

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(cameras.remove(created.id)),
                  std::runtime_error);
  CHECK(liveCameras("Renamed Cam") == 1);

  sink.refuse(false);
  CHECK(drogon::sync_wait(cameras.remove(created.id)));
  CHECK(liveCameras("Renamed Cam") == 0);

  NatsCameraChangeSink durableSink(
      nullptr, NatsCameraChangeSink::Config{.retryMs = 20,
                                            .publishSubject = {},
                                            .streamName = {}});
  camera_change::setSink(&durableSink);

  ChangeOutboxRepository outbox;
  const auto durable =
      drogon::sync_wait(cameras.create(cameraBody("Durable Cam")));
  CHECK(durable.id > 0);
  const auto pending = outbox.pendingBatch(10);
  REQUIRE(pending.size() == 1);
  CHECK(pending.front().payload.find(std::to_string(durable.id)) !=
        std::string::npos);

  DbService::cameraClient()->execSqlSync("DROP TABLE change_outbox");

  CHECK_THROWS(drogon::sync_wait(cameras.create(cameraBody("Orphan Cam"))));
  CHECK(liveCameras("Orphan Cam") == 0);
  CHECK(liveCameras("Durable Cam") == 1);

  UpdateCameraDto orphan;
  orphan.name = "Orphan Cam";
  CHECK_THROWS(drogon::sync_wait(cameras.update(durable.id, orphan)));
  CHECK(cameraName(durable.id) == "Durable Cam");

  CHECK_THROWS(drogon::sync_wait(cameras.remove(durable.id)));
  CHECK(liveCameras("Durable Cam") == 1);

  camera_change::setSink(nullptr);
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
}
