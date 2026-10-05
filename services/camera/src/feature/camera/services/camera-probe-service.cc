#include "camera-probe-service.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/camera/infra/rtsp-probe.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/services/camera-driver/tapo-driver.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>

#include <string>
#include <string_view>

namespace
{
constexpr int kProbeTimeoutMs = 4000;

struct StepInput
{
  std::string_view id;
  std::string_view status;
  std::string_view code;
  std::string detail;
};

Json::Value step(const StepInput& input)
{
  Json::Value out(Json::objectValue);
  out["id"] = std::string(input.id);
  out["status"] = std::string(input.status);
  out["code"] = std::string(input.code);
  out["detail"] = input.detail;
  return out;
}

Json::Value streamStep(std::string_view id, const RtspProbeResult& result)
{
  const bool ok = result.outcome == RtspProbeOutcome::Ok;
  return step({.id = id,
               .status = ok ? "ok" : "failed",
               .code = rtspProbeOutcomeToString(result.outcome),
               .detail = result.detail});
}

bool networkFailed(const RtspProbeResult& result)
{
  return result.outcome == RtspProbeOutcome::Unreachable ||
         result.outcome == RtspProbeOutcome::Refused;
}

std::string pathOr(const std::string& path, const char* fallback)
{
  return path.empty() ? std::string(fallback) : path;
}
}

Json::Value camera_probe::run(const CameraSchema& camera)
{
  const CameraStreamPaths paths = camera_stream_paths::of(camera.config);
  Json::Value steps(Json::arrayValue);
  Json::Value stream(Json::objectValue);
  Json::Value device(Json::objectValue);

  const auto main = rtsp_probe::describe({.host = camera.ip,
                                          .port = camera.port,
                                          .username = camera.username,
                                          .password = camera.password,
                                          .path = paths.main,
                                          .timeoutMs = kProbeTimeoutMs});
  const bool reachable = !networkFailed(main);
  steps.append(step({.id = "network",
                     .status = reachable ? "ok" : "failed",
                     .code = reachable ? "ok" : rtspProbeOutcomeToString(main.outcome),
                     .detail = reachable ? std::string() : main.detail}));
  steps.append(reachable ? streamStep("main", main)
                         : step({.id = "main", .status = "skipped", .code = "", .detail = {}}));

  stream["videoCodec"] = main.videoCodec;
  stream["audioCodec"] = main.audioCodec;
  stream["width"] = main.width;
  stream["height"] = main.height;

  if (reachable && main.outcome != RtspProbeOutcome::AuthFailed) {
    const auto sub = rtsp_probe::describe({.host = camera.ip,
                                           .port = camera.port,
                                           .username = camera.username,
                                           .password = camera.password,
                                           .path = paths.sub,
                                           .timeoutMs = kProbeTimeoutMs});
    steps.append(streamStep("sub", sub));
    stream["subWidth"] = sub.width;
    stream["subHeight"] = sub.height;
  }
  else {
    steps.append(step({.id = "sub", .status = "skipped", .code = "", .detail = {}}));
  }

  if (camera.driver == CameraDriver::Tapo) {
    if (!reachable) {
      steps.append(step({.id = "device", .status = "skipped", .code = "", .detail = {}}));
    }
    else if (camera.password.empty() && camera.cloudPassword.empty()) {
      steps.append(step({.id = "device", .status = "skipped", .code = "no_credentials", .detail = {}}));
    }
    else {
      const auto probed = TapoDriver::probe(camera);
      steps.append(step({.id = "device",
                         .status = probed.ok ? "ok" : "failed",
                         .code = probed.ok ? "ok" : probed.data.get("reason", "error").asString(),
                         .detail = probed.ok ? std::string() : probed.error}));
      if (probed.ok)
        device = probed.data;
    }
    steps.append(step({.id = "talk",
                       .status = camera.cloudPassword.empty() ? "warning" : "ok",
                       .code = camera.cloudPassword.empty() ? "cloud_password_missing" : "ok",
                       .detail = {}}));
  }

  Json::Value out(Json::objectValue);
  out["ok"] = main.outcome == RtspProbeOutcome::Ok;
  out["steps"] = steps;
  out["stream"] = stream;
  out["device"] = device;
  const std::string model = device.get("model", "").asString();
  const auto* entry = model.empty() ? nullptr
                                    : camera_catalog::find({.catalogId = {},
                                                            .driver = camera.driver,
                                                            .model = model});
  out["catalogId"] = entry == nullptr ? std::string() : std::string(entry->id);
  return out;
}

bool camera_probe::reusesStoredSecrets(const ProbeCameraDto& body)
{
  return body.cameraId.has_value() && (body.password.empty() || body.cloudPassword.empty());
}

bool camera_probe::storedAddressMatches(const StoredSecretsUse& use)
{
  return use.body.ip == use.stored.ip && use.body.port == use.stored.port;
}

ProbeSlots::Slot::~Slot()
{
  if (owner_ == nullptr)
    return;
  std::scoped_lock lock(owner_->mutex_);
  owner_->busy_.erase(userId_);
}

std::optional<ProbeSlots::Slot> ProbeSlots::acquire(int64_t userId)
{
  std::scoped_lock lock(mutex_);
  if (!busy_.insert(userId).second)
    return std::nullopt;
  return std::optional<Slot>(std::in_place, *this, userId);
}

ProbeSlots& CameraProbeService::slots()
{
  static ProbeSlots probes;
  return probes;
}

drogon::Task<Json::Value> CameraProbeService::probe(CameraProbeRequest request) const
{
  auto slot = slots().acquire(request.userId);
  if (!slot)
    throw ResponseException(CameraErrors::ProbeBusy);
  const ProbeCameraDto& body = request.body;
  CameraSchema camera;
  if (camera_probe::reusesStoredSecrets(body)) {
    if (const auto stored = co_await repository_.findById(*body.cameraId)) {
      if (!camera_probe::storedAddressMatches({.body = body, .stored = *stored}))
        throw ResponseException(CameraErrors::StoredCredentialsElsewhere);
      camera.password = stored->password;
      camera.cloudPassword = stored->cloudPassword;
      camera.tlsFingerprint = stored->tlsFingerprint;
      camera.tapoSecure = stored->tapoSecure;
    }
  }
  camera.id = body.cameraId.value_or(0);
  camera.driver = cameraDriverFromString(body.driver);
  camera.ip = body.ip;
  camera.port = body.port;
  camera.username = body.username;
  if (!body.password.empty())
    camera.password = body.password;
  camera.cloudUsername = body.cloudUsername;
  if (!body.cloudPassword.empty())
    camera.cloudPassword = body.cloudPassword;
  camera.config = camera_stream_paths::withConfig({.config = "{}",
                                                   .main = pathOr(body.streamPath, camera_stream_paths::kDefaultMain),
                                                   .sub = pathOr(body.subStreamPath, camera_stream_paths::kDefaultSub),
                                                   .catalogId = std::nullopt,
                                                   .retentionIncident = std::nullopt});
  co_return co_await BlockingTask<Json::Value>([camera]() { return camera_probe::run(camera); });
}
