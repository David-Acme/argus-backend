#include "maintenance-service.hxx"

#include <camera/camera-action-client.hxx>
#include <errors/response-exception.hxx>
#include <feature/guard/controllers/guard-errors.hxx>
#include <feature/guard/guard-tamper-policy.hxx>
#include <feature/guard/services/targeted-notice.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <utility>

namespace
{
constexpr int64_t kRecentSeconds = 86400;
constexpr int64_t kSecondsPerMinute = 60;

bool household(UserRole role)
{
  return role == UserRole::Owner || role == UserRole::Resident;
}
}

MaintenanceService::MaintenanceService(MaintenanceDependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

int64_t MaintenanceService::now() const
{
  return dependencies_.clock ? dependencies_.clock() : static_cast<int64_t>(std::time(nullptr));
}

MaintenanceConfig MaintenanceService::config() const
{
  return dependencies_.config ? dependencies_.config() : MaintenanceConfig{};
}

drogon::Task<std::vector<ResponseUser>> MaintenanceService::users() const
{
  if (!dependencies_.directory)
    co_return std::vector<ResponseUser>{};
  const auto directory = dependencies_.directory;
  const auto everyone = co_await BlockingTask<std::optional<std::vector<ResponseUser>>>(
      [directory]() { return directory->users(); });
  co_return everyone.value_or(std::vector<ResponseUser>{});
}

drogon::Task<MaintenanceWindow> MaintenanceService::require(int64_t id) const
{
  auto window = co_await repository_.findById(id);
  if (!window)
    throw ResponseException(GuardErrors::MaintenanceNotFound);
  co_return std::move(*window);
}

drogon::Task<Json::Value> MaintenanceService::list(const MaintenanceActor& actor) const
{
  const int64_t at = now();
  Json::Value windows(Json::arrayValue);
  for (const MaintenanceWindow& window : co_await repository_.recent(at - kRecentSeconds))
    windows.append(window.toJson({.owner = actor.role == UserRole::Owner, .now = at}));
  Json::Value info(Json::objectValue);
  info["windows"] = windows;
  co_return info;
}

drogon::Task<MaintenanceWindow> MaintenanceService::open(const MaintenanceDeclareInput& input) const
{
  const MaintenanceActor& actor = input.actor;
  if (!household(actor.role) && actor.role != UserRole::Guard)
    throw ResponseException(GuardErrors::MaintenanceForbidden);
  const auto assignments = co_await environmentRepository_.cameraAssignments();
  const int64_t fallbackEnvironment = co_await environmentRepository_.defaultId();
  std::map<int64_t, int64_t> environmentOf;
  for (const CameraAssignment& assignment : assignments)
    environmentOf[assignment.cameraId] = assignment.environmentId;
  std::vector<int64_t> cameras = input.cameraIds;
  if (input.environmentId > 0)
    for (const CameraAssignment& assignment : assignments)
      if (assignment.environmentId == input.environmentId)
        cameras.push_back(assignment.cameraId);
  std::ranges::sort(cameras);
  const auto [first, last] = std::ranges::unique(cameras);
  cameras.erase(first, last);
  std::erase_if(cameras, [](int64_t id) { return id <= 0; });
  if (cameras.empty())
    throw ResponseException(GuardErrors::MaintenanceCamerasRequired);
  const auto environmentFor = [&environmentOf, fallbackEnvironment](int64_t cameraId) {
    const auto found = environmentOf.find(cameraId);
    return found == environmentOf.end() || found->second <= 0 ? fallbackEnvironment : found->second;
  };
  if (actor.role == UserRole::Guard) {
    const std::vector<int64_t> duty = co_await responseRepository_.dutyEnvironments(actor.userId);
    if (duty.empty())
      throw ResponseException(GuardErrors::MaintenanceOffDuty);
    for (const int64_t cameraId : cameras)
      if (std::ranges::find(duty, environmentFor(cameraId)) == duty.end())
        throw ResponseException(GuardErrors::MaintenanceOutOfScope);
  }
  const MaintenanceConfig limits = config();
  const int64_t minutes = std::clamp<int64_t>(input.minutes > 0 ? input.minutes : limits.defaultMinutes, 1,
                                              limits.maxMinutes);
  const int64_t at = now();
  const bool duress =
      co_await repository_.recentDuress({.userId = actor.userId, .since = at - limits.duressWindowS});
  std::string name;
  for (const ResponseUser& user : co_await users())
    if (user.userId == actor.userId)
      name = user.name;
  const MaintenanceWindow window = co_await repository_.create(
      {.environmentId = input.environmentId > 0 ? input.environmentId : environmentFor(cameras.front()),
       .cameraIds = cameras,
       .declaredBy = actor.userId,
       .declaredRole = actor.role,
       .declaredName = name,
       .source = input.source,
       .duress = duress,
       .startedAt = at,
       .endsAt = at + minutes * kSecondsPerMinute});
  LOG_INFO << "Guard maintenance: window " << window.id << " by user " << actor.userId << " covers "
           << cameras.size() << " camera(s) for " << minutes << " min"
           << (duress ? " (declared under duress: it suppresses nothing)" : "");
  co_await announce({.window = window, .action = UserAction::Create, .actorId = actor.userId});
  if (duress)
    co_await notifyDuress(window);
  co_return window;
}

drogon::Task<Json::Value> MaintenanceService::declare(const MaintenanceDeclareInput& input) const
{
  const MaintenanceWindow window = co_await open(input);
  co_return window.toJson({.owner = input.actor.role == UserRole::Owner, .now = now()});
}

drogon::Task<void> MaintenanceService::announce(const AuditInput& input) const
{
  if (dependencies_.changes == nullptr)
    co_return;
  co_await dependencies_.changes->publishAction(
      {.event = {.userId = input.actorId,
                 .recordId = input.window.id,
                 .tableName = TableName::Camera,
                 .action = input.action,
                 .oldData = Json::Value(),
                 .newData = input.window.toJson({.owner = true, .now = now()}),
                 .ipAddress = {},
                 .module = "surveillance",
                 .subject = "guard_maintenance"},
       .client = nullptr});
  co_await dependencies_.changes->record(
      {.scope = ResourceScope::Maintenance, .ids = {input.window.id}, .user = 0, .client = nullptr,
       .domain = ResourceDomain::Guard});
}

drogon::Task<void> MaintenanceService::notifyDuress(const MaintenanceWindow& window) const
{
  std::vector<ResponseUser> owners;
  for (const ResponseUser& user : co_await users())
    if (user.active && user.role == UserRole::Owner && user.userId != window.declaredBy)
      owners.push_back(user);
  GuardNotice notice;
  notice.kind = NoticeKind::MaintenanceDuress;
  notice.danger = GuardDanger::Critical;
  notice.actorName = window.declaredName;
  notice.maintenanceId = window.id;
  Json::Value data(Json::objectValue);
  data["maintenanceId"] = static_cast<Json::Int64>(window.id);
  data["environmentId"] = static_cast<Json::Int64>(window.environmentId);
  data["threadKey"] = "guard:maintenance:" + std::to_string(window.id);
  static_cast<void>(co_await targeted_notice::send({.client = dependencies_.notifications,
                                                    .users = std::move(owners),
                                                    .notice = notice,
                                                    .data = data,
                                                    .commandId = "maintenance:" + std::to_string(window.id) +
                                                                 ":duress"}));
}

drogon::Task<void> MaintenanceService::close(const CloseInput& input) const
{
  const int64_t at = now();
  if (!co_await repository_.end({.id = input.window.id, .reason = input.reason, .at = at}))
    co_return;
  MaintenanceWindow window = input.window;
  window.endedAt = at;
  window.endReason = input.reason;
  for (const int64_t cameraId : window.cameraIds) {
    const std::string status = dependencies_.healthOf ? dependencies_.healthOf(cameraId) : std::string();
    if (!guard_tamper::indicating(status, true))
      continue;
    window.viewChecks.push_back({.cameraId = cameraId,
                                 .cameraName = dependencies_.cameraName ? dependencies_.cameraName(cameraId)
                                                                        : std::string(),
                                 .state = ViewCheckState::Pending});
  }
  if (!window.viewChecks.empty()) {
    co_await repository_.setViewChecks({.id = window.id, .checks = window.viewChecks});
    std::vector<ResponseUser> declarer;
    for (const ResponseUser& user : co_await users())
      if (user.userId == window.declaredBy && user.active)
        declarer.push_back(user);
    for (const ViewCheck& check : window.viewChecks) {
      GuardNotice notice;
      notice.kind = NoticeKind::ViewCheck;
      notice.cameraId = check.cameraId;
      notice.cameraName = check.cameraName;
      notice.danger = GuardDanger::Medium;
      notice.maintenanceId = window.id;
      Json::Value data(Json::objectValue);
      data["maintenanceId"] = static_cast<Json::Int64>(window.id);
      data["cameraId"] = static_cast<Json::Int64>(check.cameraId);
      data["cameraName"] = check.cameraName;
      data["threadKey"] = "guard:maintenance:" + std::to_string(window.id);
      static_cast<void>(co_await targeted_notice::send(
          {.client = dependencies_.notifications,
           .users = declarer,
           .notice = notice,
           .data = data,
           .commandId = "maintenance:" + std::to_string(window.id) + ":view:" + std::to_string(check.cameraId)}));
    }
  }
  LOG_INFO << "Guard maintenance: window " << window.id << " ended ("
           << maintenanceEndReasonToString(input.reason) << "), " << window.viewChecks.size()
           << " camera(s) look different";
  co_await announce({.window = window, .action = UserAction::Update, .actorId = input.actorId});
}

drogon::Task<Json::Value> MaintenanceService::end(const MaintenanceEndRequest& input) const
{
  const MaintenanceWindow window = co_await require(input.id);
  if (input.actor.role == UserRole::Guard && window.declaredBy != input.actor.userId)
    throw ResponseException(GuardErrors::MaintenanceNotYours);
  if (!household(input.actor.role) && input.actor.role != UserRole::Guard)
    throw ResponseException(GuardErrors::MaintenanceForbidden);
  if (window.endedAt == 0)
    co_await close({.window = window, .reason = MaintenanceEndReason::Done, .actorId = input.actor.userId});
  const MaintenanceWindow ended = co_await require(input.id);
  co_return ended.toJson({.owner = input.actor.role == UserRole::Owner, .now = now()});
}

drogon::Task<Json::Value> MaintenanceService::answerView(const MaintenanceViewInput& input) const
{
  MaintenanceWindow window = co_await require(input.id);
  if (input.actor.role != UserRole::Owner && window.declaredBy != input.actor.userId)
    throw ResponseException(GuardErrors::MaintenanceNotYours);
  const auto check = std::ranges::find_if(window.viewChecks, [&input](const ViewCheck& candidate) {
    return candidate.cameraId == input.cameraId && candidate.state == ViewCheckState::Pending;
  });
  if (check == window.viewChecks.end())
    throw ResponseException(GuardErrors::ViewCheckNotPending);
  check->state = input.accept ? ViewCheckState::Accepted : ViewCheckState::Refused;
  if (input.accept && dependencies_.actions != nullptr) {
    CameraActionClient* actions = dependencies_.actions;
    const int64_t cameraId = input.cameraId;
    const bool relearned = co_await BlockingTask<bool>([actions, cameraId]() { return actions->relearn(cameraId); });
    if (!relearned)
      LOG_WARN << "Guard maintenance: camera " << cameraId << " did not take its new view as the reference";
  }
  co_await repository_.setViewChecks({.id = window.id, .checks = window.viewChecks});
  co_await announce({.window = window, .action = UserAction::Update, .actorId = input.actor.userId});
  co_return window.toJson({.owner = input.actor.role == UserRole::Owner, .now = now()});
}

drogon::Task<Json::Value> MaintenanceService::claimIncident(const IncidentClaimInput& input) const
{
  const auto incident = co_await repository_.incident(input.incidentId);
  if (!incident)
    throw ResponseException(GuardErrors::EpisodeNotFound);
  if (incident->rule != "camera_tamper" || incident->cameraId <= 0)
    throw ResponseException(GuardErrors::NotATamper);
  const MaintenanceWindow window = co_await open({.actor = input.actor,
                                                  .cameraIds = {incident->cameraId},
                                                  .environmentId = 0,
                                                  .minutes = 0,
                                                  .source = MaintenanceSource::Alert});
  co_return window.toJson({.owner = input.actor.role == UserRole::Owner, .now = now()});
}

drogon::Task<Json::Value> MaintenanceService::answerAsk(const TamperAskReply& input) const
{
  const auto ask = co_await repository_.findAsk(input.askId);
  if (!ask || ask->answer != TamperAskAnswer::Pending)
    throw ResponseException(GuardErrors::TamperAskNotFound);
  if (!household(input.actor.role))
    throw ResponseException(GuardErrors::MaintenanceForbidden);
  if (input.answer == TamperAskAnswer::Me)
    static_cast<void>(co_await open({.actor = input.actor,
                                     .cameraIds = {ask->cameraId},
                                     .environmentId = 0,
                                     .minutes = 0,
                                     .source = MaintenanceSource::Reply}));
  const bool answered = co_await repository_.answerAsk(
      {.id = ask->id, .answer = input.answer, .answeredBy = input.actor.userId, .at = now()});
  if (!answered)
    throw ResponseException(GuardErrors::TamperAskNotFound);
  Json::Value info(Json::objectValue);
  info["answered"] = true;
  co_return info;
}

drogon::Task<std::vector<MaintenanceWindow>> MaintenanceService::activeWindows(int64_t at) const
{
  std::vector<MaintenanceWindow> active;
  for (MaintenanceWindow& window : co_await repository_.open())
    if (window.active(at))
      active.push_back(std::move(window));
  co_return active;
}

drogon::Task<void> MaintenanceService::endForUnknownPerson(const UnknownSighting& sighting) const
{
  for (const MaintenanceWindow& window : co_await activeWindows(sighting.at)) {
    if (window.environmentId != sighting.environmentId)
      continue;
    LOG_WARN << "Guard maintenance: an unknown person is in environment " << sighting.environmentId
             << "; window " << window.id << " ends now";
    co_await close({.window = window, .reason = MaintenanceEndReason::UnknownPerson, .actorId = 0});
  }
}

drogon::Task<void> MaintenanceService::expireDue(int64_t at) const
{
  for (const MaintenanceWindow& window : co_await repository_.open())
    if (window.endsAt <= at)
      co_await close({.window = window, .reason = MaintenanceEndReason::Expired, .actorId = 0});
}
