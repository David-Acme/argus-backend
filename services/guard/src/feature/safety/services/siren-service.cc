#include "siren-service.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/safety/safety-errors.hxx>
#include <json/value.h>
#include <runtime/blocking-task.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>

#include <ctime>
#include <utility>

namespace
{
int64_t secondsOf(int64_t millis)
{
  return millis > 0 ? millis / 1000 : static_cast<int64_t>(std::time(nullptr));
}

std::string commandIdFor(const AppSirenInput& input, int64_t millis)
{
  return "app-siren:" + std::to_string(input.actorUserId) + ":" +
         std::to_string(millis);
}

Json::Value auditDetail(const AppSirenInput& input, int64_t at)
{
  Json::Value detail(Json::objectValue);
  detail["event"] = "app_siren";
  detail["cameraId"] = static_cast<Json::Int64>(input.cameraId);
  detail["enabled"] = input.enabled;
  detail["at"] = static_cast<Json::Int64>(at);
  return detail;
}
}

SirenService::SirenService(Dependencies dependencies, GuardSirenConfig config)
    : dependencies_(std::move(dependencies)), config_(config)
{
}

void SirenService::refresh(GuardSirenConfig config)
{
  const std::scoped_lock lock(mutex_);
  config_ = config;
}

GuardSirenConfig SirenService::config() const
{
  const std::scoped_lock lock(mutex_);
  return config_;
}

drogon::Task<AppSirenResult> SirenService::apply(const AppSirenInput& input) const
{
  const GuardSirenConfig current = config();
  if (!current.enabled)
    throw ResponseException(SafetyErrors::AppSirenDisabled);
  if (dependencies_.camera == nullptr)
    throw ResponseException(SafetyErrors::SirenUnavailable);

  const int64_t millis = dependencies_.clock ? dependencies_.clock() : -1;
  const int64_t now = secondsOf(millis);
  const int leaseSeconds = input.enabled ? current.leaseSeconds : 0;
  const int64_t until = input.enabled ? now + leaseSeconds : 0;
  const CameraSirenInput request{
      .cameraId = input.cameraId,
      .enabled = input.enabled,
      .commandId = commandIdFor(input, millis > 0 ? millis : now),
      .encounterId = 0,
      .expiresAt = input.enabled ? until + current.expiresMarginSeconds : 0,
      .leaseSeconds = leaseSeconds};

  const CameraCommandResult ack = co_await BlockingTask<CameraCommandResult>(
      [this, request]() { return dependencies_.camera->setSiren(request); });
  if (!ack.succeeded())
    throw ResponseException(
        ack.detail == "camera_not_found"
            ? CameraErrors::CameraNotFound
            : CameraErrors::CameraUnreachable.withMessage(
                  ack.detail.empty() ? ack.status.error_message() : ack.detail));

  co_await dependencies_.changes->publishAction(
      {.event = {.userId = input.actorUserId,
                 .recordId = input.cameraId,
                 .tableName = TableName::Camera,
                 .action = UserAction::Update,
                 .oldData = Json::Value(),
                 .newData = auditDetail(input, millis > 0 ? millis : now),
                 .ipAddress = input.actorIp,
                 .module = {},
                 .subject = {}},
       .client = nullptr});
  co_await dependencies_.changes->record(
      {.scope = ResourceScope::Status,
       .ids = {input.cameraId},
       .user = 0,
       .client = nullptr,
       .domain = ResourceDomain::Camera});

  co_return AppSirenResult{.cameraId = input.cameraId,
                           .enabled = input.enabled,
                           .until = until};
}
