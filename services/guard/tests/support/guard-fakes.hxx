#pragma once

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <drogon/drogon.h>
#include <identity/identity-client.hxx>
#include <map>
#include <mutex>
#include <notification/notification-client.hxx>
#include <optional>
#include <sqlite/db-service.hxx>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace guard_test
{

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

class GuardBoot
{
public:
  explicit GuardBoot(const char* stem) : db_(stem)
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, db_.path(), "default", -1});
    runner_.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH))
      throw std::runtime_error("guard schema apply failed");
  }

private:
  TempDb db_;
  std::optional<AppRunner> runner_;
};

inline std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty() || rows.front()[0].isNull())
    return {};
  return rows.front()[0].as<std::string>();
}

inline CameraCommandResult okCommand(std::string detail)
{
  CameraCommandResult result;
  result.status = grpc::Status::OK;
  result.outcome = CameraCommandOutcome::SUCCEEDED;
  result.detail = std::move(detail);
  return result;
}

class QuietCameraActions final : public CameraActionClient
{
public:
  QuietCameraActions()
      : CameraActionClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  CameraCommandResult announce(const CameraAnnounceInput&) const override
  {
    announces.fetch_add(1);
    return okCommand("sent");
  }

  CameraCommandResult listen(const CameraListenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::INDETERMINATE;
    result.detail = "capture_failed";
    return result;
  }

  CameraCommandResult alarm(const CameraAlarmInput&) const override
  {
    alarms.fetch_add(1);
    return okCommand("alarmed");
  }

  CameraCommandResult setSiren(const CameraSirenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::REJECTED;
    result.detail = "siren_off";
    return result;
  }

  std::optional<CameraCrop>
  personCrop(const CameraPersonCropInput&) const override
  {
    return std::nullopt;
  }

  mutable std::atomic<int> announces{0};
  mutable std::atomic<int> alarms{0};
};

class RosterIdentity final : public IdentityClient
{
public:
  explicit RosterIdentity(std::map<int64_t, std::string> langs)
      : IdentityClient("127.0.0.1:1", "fleet"), langs_(std::move(langs))
  {
  }

  std::optional<std::vector<int64_t>> listNotifiableUsers() const override
  {
    std::vector<int64_t> ids;
    ids.reserve(langs_.size());
    for (const auto& [userId, lang] : langs_)
      ids.push_back(userId);
    return ids;
  }

  std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const override
  {
    const auto found = langs_.find(userId);
    if (found == langs_.end())
      return std::nullopt;
    argus::identity::v1::GetUserResponse response;
    response.mutable_user()->set_user_id(userId);
    response.mutable_user()->set_lang(found->second);
    return response;
  }

private:
  std::map<int64_t, std::string> langs_;
};

struct SentNotification
{
  std::string commandId;
  std::string title;
  std::string body;
  std::string data;
  int recipients{0};
};

class RecordingNotifications final : public NotificationClient
{
public:
  RecordingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity&) const override
  {
    {
      std::scoped_lock lock(mutex_);
      sent_.push_back({.commandId = request.command_id(),
                       .title = request.title(),
                       .body = request.body(),
                       .data = request.data(),
                       .recipients = request.user_ids_size()});
    }
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  std::vector<SentNotification> sent() const
  {
    std::scoped_lock lock(mutex_);
    return sent_;
  }

  void clear() const
  {
    std::scoped_lock lock(mutex_);
    sent_.clear();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<SentNotification> sent_;
};

}
