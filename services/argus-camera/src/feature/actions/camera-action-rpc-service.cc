#include "camera-action-rpc-service.hxx"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <drogon/drogon.h>
#include <feature/actions/audio-capture.hxx>
#include <feature/api/camera-control/dtos/camera-talk-dto.hxx>
#include <feature/api/camera-control/services/camera-control-feature-service.hxx>
#include <optional>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/utils/sha256/sha256.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <vector>

namespace
{

constexpr int64_t kCommandLeaseSeconds = 120;

int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}

std::vector<int16_t> sirenTone(int seconds)
{
  constexpr int kSampleRate = 16000;
  constexpr double kTwoPi = 6.283185307179586;
  const int bounded = std::clamp(seconds, 1, 10);
  std::vector<int16_t> samples;
  samples.reserve(static_cast<size_t>(kSampleRate) * bounded);
  double phase = 0.0;
  for (int i = 0; i < kSampleRate * bounded; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double frequency = std::fmod(t, 1.0) < 0.5 ? 700.0 : 1000.0;
    phase += kTwoPi * frequency / kSampleRate;
    samples.push_back(static_cast<int16_t>(std::sin(phase) * 16000.0));
  }
  return samples;
}

// Unambiguous length-prefixed digest so adjacent fields cannot collide.
std::string commandFingerprint(const std::vector<std::string>& fields)
{
  argus::hash::Sha256 hasher;
  for (const std::string& field : fields) {
    hasher.update(std::to_string(field.size()));
    hasher.update(":");
    hasher.update(field);
    hasher.update("\n");
  }
  return argus::hash::hex(hasher.digest());
}

struct AckInput
{
  argus::camera::v1::ActionAck* response{nullptr};
  CameraCommandOutcome outcome{
      CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED};
  std::string detail;
};

void finishAck(const AckInput& input)
{
  input.response->set_outcome(input.outcome);
  input.response->set_accepted(
      input.outcome == CameraCommandOutcome::SUCCEEDED ||
      input.outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED);
  input.response->set_duplicate(input.outcome ==
                                CameraCommandOutcome::DUPLICATE_SUCCEEDED);
  input.response->set_detail(input.detail);
}

// Replays a stored ListenResponse for a duplicate command id.
void applyListenResponse(argus::camera::v1::ListenResponse* response,
                         const std::string& json)
{
  const Json::Value stored = json_util::fromString(json);
  if (!stored.isObject())
    return;
  response->set_captured(stored.get("captured", false).asBool());
  response->set_speech_detected(stored.get("speechDetected", false).asBool());
  response->set_endpointed(stored.get("endpointed", false).asBool());
  response->set_text(stored.get("text", "").asString());
}

} // namespace

CameraActionRpcService::CameraActionRpcService(CameraActionServiceConfig config)
    : callers_(std::move(config.callers)),
      transcriber_(std::move(config.transcriber))
{
}

bool CameraActionRpcService::authorized(
    const grpc::CallbackServerContext* context) const
{
  return argus::sdk::authorizeCaller(context, callers_).has_value();
}

bool CameraActionRpcService::actionsEnabled() const
{
  return ConfigService::getBool("actions.enabled");
}

drogon::Task<CameraActionRpcService::SettleVerdict>
CameraActionRpcService::settleFenced(const CommandSettleInput& input)
{
  SettleVerdict verdict;
  if (co_await settleCommand(input)) {
    verdict.won = true;
    co_return verdict;
  }
  const ActionCommandRow row =
      co_await commandRepository_.find(input.commandId);
  if (!row.found) {
    verdict.outcome = CameraCommandOutcome::INDETERMINATE;
    verdict.detail = "settle_lost";
    co_return verdict;
  }
  verdict.detail = row.detail;
  verdict.response = row.response;
  if (row.status == "succeeded")
    verdict.outcome = CameraCommandOutcome::SUCCEEDED;
  else if (row.status == "rejected")
    verdict.outcome = CameraCommandOutcome::REJECTED;
  else if (row.status == "retryable_failed")
    verdict.outcome = CameraCommandOutcome::RETRYABLE_FAILED;
  else if (row.status == "executing")
    verdict.outcome = CameraCommandOutcome::IN_FLIGHT;
  else
    verdict.outcome = CameraCommandOutcome::INDETERMINATE;
  co_return verdict;
}

bool CameraActionRpcService::migrateActionSchema()
{
  return commandRepository_.migrateLegacySchema();
}

drogon::Task<CameraActionRpcService::CommandVerdict>
CameraActionRpcService::beginCommand(const CommandContext& input)
{
  CommandVerdict verdict;
  if (input.commandId.empty()) {
    verdict.outcome = CameraCommandOutcome::REJECTED;
    verdict.detail = "command_id_required";
    co_return verdict;
  }
  const ActionClaim claim =
      co_await commandRepository_.claim({.commandId = input.commandId,
                                         .kind = input.kind,
                                         .cameraId = input.cameraId,
                                         .fingerprint = input.fingerprint,
                                         .at = nowSeconds(),
                                         .leaseSeconds = input.leaseSeconds});
  switch (claim.kind) {
    case ActionClaimKind::Completed:
      verdict.outcome = CameraCommandOutcome::DUPLICATE_SUCCEEDED;
      verdict.detail = claim.detail.empty() ? "duplicate" : claim.detail;
      verdict.response = claim.response;
      co_return verdict;
    case ActionClaimKind::InFlight:
      verdict.outcome = CameraCommandOutcome::IN_FLIGHT;
      verdict.detail = claim.detail.empty() ? "in_flight" : claim.detail;
      co_return verdict;
    case ActionClaimKind::Indeterminate:
      verdict.outcome = CameraCommandOutcome::INDETERMINATE;
      verdict.detail = claim.detail.empty() ? "indeterminate" : claim.detail;
      co_return verdict;
    case ActionClaimKind::Conflict:
      verdict.outcome = CameraCommandOutcome::CONFLICT;
      verdict.detail = "command_id_conflict";
      co_return verdict;
    case ActionClaimKind::Rejected:
      verdict.outcome = CameraCommandOutcome::REJECTED;
      verdict.detail = claim.detail.empty() ? "rejected" : claim.detail;
      co_return verdict;
    case ActionClaimKind::New:
    case ActionClaimKind::Retry:
      break;
  }
  if (input.expiresAt > 0 && nowSeconds() > input.expiresAt) {
    co_await commandRepository_.settle({.commandId = input.commandId,
                                        .generation = claim.generation,
                                        .status = "rejected",
                                        .detail = "expired",
                                        .response = {},
                                        .at = nowSeconds()});
    verdict.outcome = CameraCommandOutcome::REJECTED;
    verdict.detail = "expired";
    co_return verdict;
  }
  verdict.proceed = true;
  verdict.generation = claim.generation;
  verdict.detail = claim.kind == ActionClaimKind::Retry ? "retry" : "new";
  co_return verdict;
}

drogon::Task<bool>
CameraActionRpcService::settleCommand(const CommandSettleInput& input)
{
  if (input.commandId.empty())
    co_return true;
  co_return co_await commandRepository_.settle({.commandId = input.commandId,
                                                .generation = input.generation,
                                                .status = input.status,
                                                .detail = input.detail,
                                                .response = input.response,
                                                .at = nowSeconds()});
}

grpc::ServerUnaryReactor* CameraActionRpcService::Announce(
    grpc::CallbackServerContext* context,
    const argus::camera::v1::AnnounceRequest* request,
    argus::camera::v1::ActionAck* response)
{
  if (!authorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "caller capability credential invalid"));
    return reactor;
  }
  if (!actionsEnabled()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                 "Camera actions are disabled"));
    return reactor;
  }
  if (request->camera_id() <= 0 || request->text().empty() ||
      request->text().size() > 300 || request->command_id().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "camera_id, command_id and a bounded text "
                                 "are required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const std::string commandId = request->command_id();
  const std::string text = request->text();
  const std::string lang = request->lang().empty() ? "es" : request->lang();
  const int64_t encounterId = request->encounter_id();
  const int64_t expiresAt = request->expires_at();
  CameraTalkDto dto;
  dto.text = text;
  dto.lang = lang;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter, cameraId,
                                        commandId, text, lang, encounterId,
                                        expiresAt, dto]() {
    drogon::async_run([this, reactor, responseWriter, cameraId, commandId, text,
                       lang, encounterId, expiresAt,
                       dto]() -> drogon::Task<void> {
      const std::string fingerprint =
          commandFingerprint({"announce", std::to_string(cameraId),
                              std::to_string(encounterId), text, lang});
      const auto verdict =
          co_await beginCommand({.commandId = commandId,
                                 .kind = "announce",
                                 .cameraId = cameraId,
                                 .encounterId = encounterId,
                                 .expiresAt = expiresAt,
                                 .leaseSeconds = kCommandLeaseSeconds,
                                 .fingerprint = fingerprint});
      if (!verdict.proceed) {
        finishAck({.response = responseWriter,
                   .outcome = verdict.outcome,
                   .detail = verdict.detail});
        reactor->Finish(grpc::Status::OK);
        co_return;
      }
      bool dispatched = false;
      bool failed = false;
      std::string failure;
      try {
        dispatched = true;
        const auto result = co_await cameraControlService_.speak(cameraId, dto);
        if (!result) {
          const auto fence =
              co_await settleFenced({.commandId = commandId,
                                     .generation = verdict.generation,
                                     .status = "rejected",
                                     .detail = "camera_not_found",
                                     .response = {}});
          finishAck({.response = responseWriter,
                     .outcome = fence.won ? CameraCommandOutcome::REJECTED
                                          : fence.outcome,
                     .detail = fence.won ? "camera_not_found" : fence.detail});
          reactor->Finish(grpc::Status::OK);
          co_return;
        }
        dispatched = result->attempted;
        const bool ok = result->ok;
        const std::string detail = result->error;
        const CameraCommandOutcome intended =
            ok ? CameraCommandOutcome::SUCCEEDED
               : (dispatched ? CameraCommandOutcome::INDETERMINATE
                             : CameraCommandOutcome::RETRYABLE_FAILED);
        const std::string status =
            ok ? "succeeded"
               : (dispatched ? "indeterminate" : "retryable_failed");
        const auto fence =
            co_await settleFenced({.commandId = commandId,
                                   .generation = verdict.generation,
                                   .status = status,
                                   .detail = detail,
                                   .response = {}});
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? detail : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera action: Announce failed: " << e.what();
        failure = e.what();
        failed = true;
      }
      if (failed) {
        const bool ambiguous = dispatched;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = ambiguous ? "indeterminate" : "retryable_failed",
             .detail = failure,
             .response = {}});
        const CameraCommandOutcome intended =
            ambiguous ? CameraCommandOutcome::INDETERMINATE
                      : CameraCommandOutcome::RETRYABLE_FAILED;
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? failure : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor*
CameraActionRpcService::Alarm(grpc::CallbackServerContext* context,
                              const argus::camera::v1::AlarmRequest* request,
                              argus::camera::v1::ActionAck* response)
{
  if (!authorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "caller capability credential invalid"));
    return reactor;
  }
  if (!actionsEnabled()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                 "Camera actions are disabled"));
    return reactor;
  }
  if (request->camera_id() <= 0 || request->command_id().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "camera_id and command_id are required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const int seconds = std::clamp(request->seconds(), 1, 10);
  const std::string commandId = request->command_id();
  const int64_t encounterId = request->encounter_id();
  const int64_t expiresAt = request->expires_at();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter, cameraId,
                                        seconds, commandId, encounterId,
                                        expiresAt]() {
    drogon::async_run([this, reactor, responseWriter, cameraId, seconds,
                       commandId, encounterId,
                       expiresAt]() -> drogon::Task<void> {
      const std::string fingerprint = commandFingerprint(
          {"alarm", std::to_string(cameraId), std::to_string(encounterId),
           std::to_string(seconds)});
      const auto verdict =
          co_await beginCommand({.commandId = commandId,
                                 .kind = "alarm",
                                 .cameraId = cameraId,
                                 .encounterId = encounterId,
                                 .expiresAt = expiresAt,
                                 .leaseSeconds = kCommandLeaseSeconds,
                                 .fingerprint = fingerprint});
      if (!verdict.proceed) {
        finishAck({.response = responseWriter,
                   .outcome = verdict.outcome,
                   .detail = verdict.detail});
        reactor->Finish(grpc::Status::OK);
        co_return;
      }
      bool dispatched = false;
      bool failed = false;
      std::string failure;
      try {
        const auto camera = co_await cameraRepository_.findById(cameraId);
        const auto driver =
            camera ? CameraDriverRegistry::instance().driverFor(*camera)
                   : nullptr;
        if (!driver) {
          const auto fence = co_await settleFenced(
              {.commandId = commandId,
               .generation = verdict.generation,
               .status = "rejected",
               .detail = camera ? "no_driver" : "camera_not_found",
               .response = {}});
          finishAck({.response = responseWriter,
                     .outcome = fence.won ? CameraCommandOutcome::REJECTED
                                          : fence.outcome,
                     .detail = fence.won
                                   ? (camera ? "no_driver" : "camera_not_found")
                                   : fence.detail});
          reactor->Finish(grpc::Status::OK);
          co_return;
        }
        const auto samples = co_await BlockingTask<std::vector<int16_t>>(
            [seconds]() { return sirenTone(seconds); });
        dispatched = true;
        const auto result =
            co_await BlockingTask<DriverResult>([driver, samples]() {
              return driver->speak({.samples = samples, .sampleRate = 16000});
            });
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = result.ok ? "succeeded" : "indeterminate",
             .detail = result.error,
             .response = {}});
        const CameraCommandOutcome intended =
            result.ok ? CameraCommandOutcome::SUCCEEDED
                      : CameraCommandOutcome::INDETERMINATE;
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? result.error : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera action: Alarm failed: " << e.what();
        failure = e.what();
        failed = true;
      }
      if (failed) {
        const bool ambiguous = dispatched;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = ambiguous ? "indeterminate" : "retryable_failed",
             .detail = failure,
             .response = {}});
        const CameraCommandOutcome intended =
            ambiguous ? CameraCommandOutcome::INDETERMINATE
                      : CameraCommandOutcome::RETRYABLE_FAILED;
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? failure : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor*
CameraActionRpcService::SetSiren(grpc::CallbackServerContext* context,
                                 const argus::camera::v1::SirenRequest* request,
                                 argus::camera::v1::ActionAck* response)
{
  if (!authorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "caller capability credential invalid"));
    return reactor;
  }
  if (!actionsEnabled()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                 "Camera actions are disabled"));
    return reactor;
  }
  if (request->camera_id() <= 0 || request->command_id().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "camera_id and command_id are required"));
    return reactor;
  }
  if (request->enabled() && request->lease_seconds() <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                     "arming the siren requires a positive lease_seconds"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const bool enabled = request->enabled();
  const int leaseSeconds = request->lease_seconds();
  const std::string commandId = request->command_id();
  const int64_t encounterId = request->encounter_id();
  const int64_t expiresAt = request->expires_at();
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter, cameraId,
                                        enabled, leaseSeconds, commandId,
                                        encounterId, expiresAt]() {
    drogon::async_run([this, reactor, responseWriter, cameraId, enabled,
                       leaseSeconds, commandId, encounterId,
                       expiresAt]() -> drogon::Task<void> {
      const std::string fingerprint = commandFingerprint(
          {"siren", std::to_string(cameraId), std::to_string(encounterId),
           enabled ? "1" : "0", std::to_string(leaseSeconds)});
      const auto verdict =
          co_await beginCommand({.commandId = commandId,
                                 .kind = "siren",
                                 .cameraId = cameraId,
                                 .encounterId = encounterId,
                                 .expiresAt = expiresAt,
                                 .leaseSeconds = kCommandLeaseSeconds,
                                 .fingerprint = fingerprint});
      if (!verdict.proceed) {
        finishAck({.response = responseWriter,
                   .outcome = verdict.outcome,
                   .detail = verdict.detail});
        reactor->Finish(grpc::Status::OK);
        co_return;
      }
      bool dispatched = false;
      bool failed = false;
      std::string failure;
      try {
        const auto camera = co_await cameraRepository_.findById(cameraId);
        const auto driver =
            camera ? CameraDriverRegistry::instance().driverFor(*camera)
                   : nullptr;
        if (!driver) {
          const auto fence = co_await settleFenced(
              {.commandId = commandId,
               .generation = verdict.generation,
               .status = "rejected",
               .detail = camera ? "no_driver" : "camera_not_found",
               .response = {}});
          finishAck({.response = responseWriter,
                     .outcome = fence.won ? CameraCommandOutcome::REJECTED
                                          : fence.outcome,
                     .detail = fence.won
                                   ? (camera ? "no_driver" : "camera_not_found")
                                   : fence.detail});
          reactor->Finish(grpc::Status::OK);
          co_return;
        }
        const int64_t now = nowSeconds();
        if (enabled) {
          const bool leased = co_await commandRepository_.upsertLease(
              {.cameraId = cameraId,
               .commandId = commandId,
               .expiresAt = now + leaseSeconds,
               .at = now});
          if (!leased) {
            LOG_ERROR << "Camera action: siren lease persistence failed "
                         "for camera "
                      << cameraId;
            const auto fence =
                co_await settleFenced({.commandId = commandId,
                                       .generation = verdict.generation,
                                       .status = "rejected",
                                       .detail = "lease_persist_failed",
                                       .response = {}});
            finishAck(
                {.response = responseWriter,
                 .outcome =
                     fence.won ? CameraCommandOutcome::REJECTED : fence.outcome,
                 .detail = fence.won ? "lease_persist_failed" : fence.detail});
            reactor->Finish(grpc::Status::OK);
            co_return;
          }
        }
        const std::optional<int> volume =
            enabled ? std::optional<int>(100) : std::nullopt;
        dispatched = true;
        const auto result =
            co_await BlockingTask<DriverResult>([driver, enabled, volume]() {
              return driver->settings({.privacy = std::nullopt,
                                       .led = std::nullopt,
                                       .dayNight = std::nullopt,
                                       .motion = std::nullopt,
                                       .motionSensitivity = std::nullopt,
                                       .autoTrack = std::nullopt,
                                       .alarm = enabled,
                                       .alarmVolume = volume});
            });
        if (!enabled && result.ok &&
            !co_await commandRepository_.deleteLease(cameraId))
          LOG_ERROR << "Camera action: siren lease delete failed for "
                       "camera "
                    << cameraId;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = result.ok ? "succeeded" : "indeterminate",
             .detail = result.error,
             .response = {}});
        const CameraCommandOutcome intended =
            result.ok ? CameraCommandOutcome::SUCCEEDED
                      : CameraCommandOutcome::INDETERMINATE;
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? result.error : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera action: SetSiren failed: " << e.what();
        failure = e.what();
        failed = true;
      }
      if (failed) {
        const bool ambiguous = dispatched;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = ambiguous ? "indeterminate" : "retryable_failed",
             .detail = failure,
             .response = {}});
        const CameraCommandOutcome intended =
            ambiguous ? CameraCommandOutcome::INDETERMINATE
                      : CameraCommandOutcome::RETRYABLE_FAILED;
        finishAck({.response = responseWriter,
                   .outcome = fence.won ? intended : fence.outcome,
                   .detail = fence.won ? failure : fence.detail});
        reactor->Finish(grpc::Status::OK);
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* CameraActionRpcService::GetPersonCrop(
    grpc::CallbackServerContext* context,
    const argus::camera::v1::PersonCropRequest* request,
    argus::camera::v1::PersonCropResponse* response)
{
  if (!authorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "caller capability credential invalid"));
    return reactor;
  }
  if (request->camera_id() <= 0) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "camera_id is required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const int64_t trackId = request->track_id();
  std::optional<CameraSnapshot> crop;
  if (trackId > 0)
    crop = SnapshotStore::instance().personCrop(cameraId, trackId);
  else {
    crop = SnapshotStore::instance().latestPersonCrop(cameraId);
    if (!crop)
      crop = SnapshotStore::instance().frame(cameraId);
  }
  if (crop)
    response->set_jpeg(crop->jpeg);
  response->set_captured_at(crop ? crop->atMs : 0);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor*
CameraActionRpcService::Listen(grpc::CallbackServerContext* context,
                               const argus::camera::v1::ListenRequest* request,
                               argus::camera::v1::ListenResponse* response)
{
  if (!authorized(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "caller capability credential invalid"));
    return reactor;
  }
  if (request->camera_id() <= 0 || request->command_id().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "camera_id and command_id are required"));
    return reactor;
  }

  const int64_t cameraId = request->camera_id();
  const int seconds = std::clamp(request->seconds(), 1, 10);
  const std::string lang =
      request->lang().empty() ? "es" : request->lang().substr(0, 8);
  const std::string commandId = request->command_id();
  const int64_t encounterId = request->encounter_id();
  const int64_t expiresAt = request->expires_at();

  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, responseWriter, cameraId,
                                        seconds, lang, commandId, encounterId,
                                        expiresAt]() {
    drogon::async_run([this, reactor, responseWriter, cameraId, seconds, lang,
                       commandId, encounterId,
                       expiresAt]() -> drogon::Task<void> {
      const std::string fingerprint = commandFingerprint(
          {"greet_listen", std::to_string(cameraId),
           std::to_string(encounterId), std::to_string(seconds), lang});
      const auto verdict =
          co_await beginCommand({.commandId = commandId,
                                 .kind = "greet_listen",
                                 .cameraId = cameraId,
                                 .encounterId = encounterId,
                                 .expiresAt = expiresAt,
                                 .leaseSeconds = kCommandLeaseSeconds,
                                 .fingerprint = fingerprint});
      if (!verdict.proceed) {
        if (verdict.outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED)
          applyListenResponse(responseWriter, verdict.response);
        responseWriter->set_outcome(verdict.outcome);
        responseWriter->set_duplicate(
            verdict.outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED);
        reactor->Finish(grpc::Status::OK);
        co_return;
      }
      bool dispatched = false;
      bool failed = false;
      std::string failure;
      try {
        const std::string url = Go2rtcManager::instance().rtspBase() + "/" +
                                Go2rtcManager::streamName(cameraId);
        dispatched = true;
        const auto captured =
            co_await BlockingTask<AudioCaptureResult>([url, seconds]() {
              return audio_capture::capture(
                  {.url = url, .seconds = seconds, .endpoint = true});
            });
        responseWriter->set_captured(captured.ok);
        responseWriter->set_speech_detected(captured.speechDetected);
        responseWriter->set_endpointed(captured.endpointed);
        responseWriter->set_duplicate(false);

        std::string text;
        if (captured.ok && transcriber_) {
          text = co_await BlockingTask<std::string>(
              [this, samples = captured.samples, lang]() {
                return transcriber_->transcribe(samples, lang);
              });
          responseWriter->set_text(text);
        }

        Json::Value stored(Json::objectValue);
        stored["captured"] = captured.ok;
        stored["speechDetected"] = captured.speechDetected;
        stored["endpointed"] = captured.endpointed;
        stored["text"] = text;
        const CameraCommandOutcome intended =
            captured.ok ? CameraCommandOutcome::SUCCEEDED
                        : CameraCommandOutcome::INDETERMINATE;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = captured.ok ? "succeeded" : "indeterminate",
             .detail = captured.ok ? "captured" : "capture_failed",
             .response = json_util::toString(stored)});
        if (!fence.won) {
          responseWriter->set_captured(false);
          responseWriter->set_speech_detected(false);
          responseWriter->set_endpointed(false);
          responseWriter->set_text({});
          responseWriter->set_outcome(fence.outcome);
        }
        else {
          responseWriter->set_outcome(intended);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera action: Listen failed: " << e.what();
        failure = e.what();
        failed = true;
      }
      if (failed) {
        const bool ambiguous = dispatched;
        const auto fence = co_await settleFenced(
            {.commandId = commandId,
             .generation = verdict.generation,
             .status = ambiguous ? "indeterminate" : "retryable_failed",
             .detail = failure,
             .response = {}});
        responseWriter->set_outcome(
            fence.won ? (ambiguous ? CameraCommandOutcome::INDETERMINATE
                                   : CameraCommandOutcome::RETRYABLE_FAILED)
                      : fence.outcome);
        reactor->Finish(grpc::Status::OK);
      }
      co_return;
    });
  });
  return reactor;
}

void CameraActionRpcService::startLeaseSweeper()
{
  drogon::app().getLoop()->runEvery(5.0, [this]() {
    drogon::async_run([this]() -> drogon::Task<void> {
      const int64_t recovered =
          co_await commandRepository_.reconcileExpired(nowSeconds(),
                                                       kCommandLeaseSeconds);
      if (recovered > 0)
        LOG_WARN << "Camera action: marked " << recovered
                 << " lost in-flight command(s) indeterminate";
      const auto expired =
          co_await commandRepository_.expiredLeases(nowSeconds());
      for (const int64_t cameraId : expired) {
        const auto camera = co_await cameraRepository_.findById(cameraId);
        if (!camera) {
          LOG_ERROR << "Camera action: siren lease for camera " << cameraId
                    << " has no camera row; keeping it and retrying";
          continue;
        }
        const auto driver = CameraDriverRegistry::instance().driverFor(*camera);
        if (!driver) {
          LOG_ERROR << "Camera action: siren lease for camera " << cameraId
                    << " has no reachable driver; keeping it and retrying";
          continue;
        }
        const auto result = co_await BlockingTask<DriverResult>([driver]() {
          return driver->settings({.privacy = std::nullopt,
                                   .led = std::nullopt,
                                   .dayNight = std::nullopt,
                                   .motion = std::nullopt,
                                   .motionSensitivity = std::nullopt,
                                   .autoTrack = std::nullopt,
                                   .alarm = false,
                                   .alarmVolume = std::nullopt});
        });
        if (!result.ok) {
          LOG_WARN << "Camera action: siren lease disarm failed for camera "
                   << cameraId << "; retrying next sweep (" << result.error
                   << ")";
          continue;
        }
        if (!co_await commandRepository_.deleteLease(cameraId)) {
          LOG_ERROR << "Camera action: siren lease delete failed for camera "
                    << cameraId << " after disarm; retrying next sweep";
          continue;
        }
        LOG_WARN << "Camera action: siren lease expired for camera " << cameraId
                 << "; disarmed";
      }
      co_return;
    });
  });
}
