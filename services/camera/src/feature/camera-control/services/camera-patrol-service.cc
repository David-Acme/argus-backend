#include "camera-patrol-service.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <sync/camera-change-sink.hxx>

#include <optional>
#include <utility>

namespace
{
std::optional<std::optional<PatrolWindow>> windowChange(const CameraPatrolDto& body)
{
  if (!body.windowGiven)
    return std::nullopt;
  return std::optional<std::optional<PatrolWindow>>(body.window);
}
}

drogon::Task<void> CameraPatrolService::requireCamera(int64_t cameraId) const
{
  if (!co_await cameraRepository_.findById(cameraId))
    throw ResponseException(CameraErrors::CameraNotFound);
}

drogon::Task<void> CameraPatrolService::requireSavedStops(int64_t cameraId,
                                                          const std::vector<std::string>& stops) const
{
  const auto listed = co_await service_.presets(cameraId);
  if (!listed)
    throw ResponseException(CameraErrors::CameraNotFound);
  if (!listed->ok)
    throw ResponseException(listed->error.empty()
                                ? CameraErrors::CameraUnreachable
                                : CameraErrors::CameraUnreachable.withMessage(listed->error));
  const auto unknown =
      patrol_plan::unknownStops({.stops = stops, .saved = patrol_plan::savedPresetIds(listed->data)});
  if (unknown.empty())
    co_return;
  std::vector<std::string> messages;
  messages.reserve(unknown.size());
  for (const std::string& stop : unknown)
    messages.push_back("preset " + stop + " is not saved on this camera");
  ValidationErrors errors;
  errors["stops"] = std::move(messages);
  throw ValidationException(std::move(errors));
}

drogon::Task<ResponseCameraPatrolDto> CameraPatrolService::read(int64_t cameraId) const
{
  co_await requireCamera(cameraId);
  co_return ResponseCameraPatrolDto{.patrol = co_await patrolRepository_.findByCamera(cameraId)};
}

drogon::Task<ResponseCameraPatrolDto> CameraPatrolService::change(const CameraPatrolChangeInput& input) const
{
  const int64_t cameraId = input.cameraId;
  const CameraPatrolDto& body = input.body;
  co_await requireCamera(cameraId);
  const auto existing = co_await patrolRepository_.findByCamera(cameraId);
  if (!existing && !body.stops) {
    ValidationErrors errors;
    errors["stops"].emplace_back("a new patrol needs its stops");
    throw ValidationException(std::move(errors));
  }
  if (body.stops)
    co_await requireSavedStops(cameraId, *body.stops);

  std::optional<CameraPatrolSchema> after;
  if (existing)
    after = co_await patrolRepository_.update(cameraId,
                                              {.stops = body.stops,
                                               .dwellSeconds = body.dwellSeconds,
                                               .isEnabled = body.enabled,
                                               .window = windowChange(body),
                                               .client = nullptr});
  else
    after = co_await patrolRepository_.create(
        {.cameraId = cameraId,
         .stops = *body.stops,
         .dwellSeconds = body.dwellSeconds.value_or(patrol_plan::kDefaultDwellSeconds),
         .isEnabled = body.enabled.value_or(true),
         .window = body.windowGiven ? body.window : std::nullopt,
         .client = nullptr});
  if (!after)
    throw ResponseException(CameraErrors::CameraNotFound);

  co_await publish({.cameraId = cameraId,
                    .action = existing ? UserAction::Update : UserAction::Create,
                    .before = existing ? existing->toJson() : Json::Value(),
                    .after = after->toJson(),
                    .actorUserId = input.actorUserId,
                    .actorIp = input.actorIp});
  co_return ResponseCameraPatrolDto{.patrol = std::move(after)};
}

drogon::Task<ResponseCameraPatrolDto> CameraPatrolService::remove(const CameraPatrolRemoveInput& input) const
{
  co_await requireCamera(input.cameraId);
  const auto existing = co_await patrolRepository_.findByCamera(input.cameraId);
  if (existing && co_await patrolRepository_.remove(input.cameraId))
    co_await publish({.cameraId = input.cameraId,
                      .action = UserAction::Delete,
                      .before = existing->toJson(),
                      .after = Json::Value(),
                      .actorUserId = input.actorUserId,
                      .actorIp = input.actorIp});
  co_return ResponseCameraPatrolDto{.patrol = std::nullopt};
}

drogon::Task<void> CameraPatrolService::publish(PatrolAudit audit)
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  Json::Value oldData(Json::objectValue);
  oldData["patrol"] = std::move(audit.before);
  Json::Value newData(Json::objectValue);
  newData["patrol"] = std::move(audit.after);
  co_await sink->publishAction({.event = {.userId = audit.actorUserId,
                                          .recordId = audit.cameraId,
                                          .tableName = TableName::Camera,
                                          .action = audit.action,
                                          .oldData = std::move(oldData),
                                          .newData = std::move(newData),
                                          .ipAddress = audit.actorIp},
                                .client = nullptr});
  co_await sink->publishResource({.scope = ResourceScope::Status,
                                  .ids = {audit.cameraId},
                                  .user = 0,
                                  .client = nullptr});
}
