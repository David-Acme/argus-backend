#pragma once

#include <auth/user-role.hxx>
#include <config/guard-config.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/maintenance-schema.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <feature/guard/repositories/maintenance/maintenance-repository.hxx>
#include <feature/guard/repositories/response/response-repository.hxx>
#include <feature/guard/services/response-plan.hxx>
#include <sync/user-action.hxx>
#include <shared/vocabulary/maintenance-voice.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CameraActionClient;
class NotificationClient;

struct MaintenanceDependencies
{
  std::shared_ptr<const ResponseDirectory> directory;
  NotificationClient* notifications{nullptr};
  CameraActionClient* actions{nullptr};
  const ResourceChangeOutbox* changes{nullptr};
  std::function<std::string(int64_t)> healthOf;
  std::function<std::string(int64_t)> cameraName;
  std::function<MaintenanceConfig()> config;
  std::function<int64_t()> clock;
};

struct MaintenanceActor
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
};

struct MaintenanceDeclareInput
{
  MaintenanceActor actor;
  std::vector<int64_t> cameraIds;
  int64_t environmentId{0};
  int64_t minutes{0};
  MaintenanceSource source{MaintenanceSource::App};
};

struct MaintenanceEndRequest
{
  MaintenanceActor actor;
  int64_t id{0};
};

struct MaintenanceViewInput
{
  MaintenanceActor actor;
  int64_t id{0};
  int64_t cameraId{0};
  bool accept{false};
};

struct IncidentClaimInput
{
  MaintenanceActor actor;
  int64_t incidentId{0};
};

struct TamperAskReply
{
  MaintenanceActor actor;
  int64_t askId{0};
  TamperAskAnswer answer{TamperAskAnswer::No};
};

struct UnknownSighting
{
  int64_t environmentId{0};
  int64_t at{0};
};

class MaintenanceService
{
public:
  explicit MaintenanceService(MaintenanceDependencies dependencies);

  [[nodiscard]] drogon::Task<Json::Value> list(const MaintenanceActor& actor) const;
  [[nodiscard]] drogon::Task<Json::Value> declare(const MaintenanceDeclareInput& input) const;
  [[nodiscard]] drogon::Task<Json::Value> end(const MaintenanceEndRequest& input) const;
  [[nodiscard]] drogon::Task<Json::Value> answerView(const MaintenanceViewInput& input) const;
  [[nodiscard]] drogon::Task<Json::Value> claimIncident(const IncidentClaimInput& input) const;
  [[nodiscard]] drogon::Task<Json::Value> answerAsk(const TamperAskReply& input) const;

  [[nodiscard]] drogon::Task<MaintenanceVoiceReply> startByVoice(MaintenanceVoiceRequest request) const;
  [[nodiscard]] drogon::Task<MaintenanceVoiceReply> endByVoice(MaintenanceVoiceRequest request) const;
  [[nodiscard]] drogon::Task<MaintenanceVoiceReply> statusByVoice(MaintenanceVoiceRequest request) const;

  [[nodiscard]] drogon::Task<std::vector<MaintenanceWindow>> activeWindows(int64_t now) const;
  [[nodiscard]] drogon::Task<void> endForUnknownPerson(const UnknownSighting& sighting) const;
  [[nodiscard]] drogon::Task<void> expireDue(int64_t now) const;

private:
  struct CloseInput
  {
    MaintenanceWindow window;
    MaintenanceEndReason reason{MaintenanceEndReason::Done};
    int64_t actorId{0};
  };

  struct AuditInput
  {
    const MaintenanceWindow& window;
    UserAction action{UserAction::Create};
    int64_t actorId{0};
  };

  [[nodiscard]] drogon::Task<MaintenanceWindow> open(const MaintenanceDeclareInput& input) const;
  [[nodiscard]] drogon::Task<void> close(const CloseInput& input) const;
  [[nodiscard]] drogon::Task<void> announce(const AuditInput& input) const;
  [[nodiscard]] drogon::Task<void> publishEnded(const MaintenanceWindow& window) const;
  [[nodiscard]] drogon::Task<std::vector<ResponseUser>> users() const;
  [[nodiscard]] drogon::Task<MaintenanceWindow> require(int64_t id) const;
  [[nodiscard]] drogon::Task<void> notifyDuress(const MaintenanceWindow& window) const;
  [[nodiscard]] int64_t now() const;
  [[nodiscard]] MaintenanceConfig config() const;

  MaintenanceDependencies dependencies_;
  MaintenanceRepository repository_;
  EnvironmentRepository environmentRepository_;
  ResponseRepository responseRepository_;
};
