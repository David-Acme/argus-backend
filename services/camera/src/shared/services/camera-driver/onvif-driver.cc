#include <shared/services/camera-driver/onvif-driver.hxx>

#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/services/camera-driver/camera-capabilities.hxx>
#include <shared/services/camera-driver/capability-record.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>

#include <cmath>
#include <numbers>
#include <utility>

namespace
{
constexpr int kDefaultPort = 80;
constexpr double kTimeoutSeconds = 5.0;
constexpr double kHoldSpeed = 0.5;
constexpr double kPanSpanDegrees = 180.0;
constexpr double kTiltSpanDegrees = 90.0;
constexpr int kHoldRenewMs = 1000;

template <typename T>
DriverResult failureOf(const OnvifOutcome<T>& outcome)
{
  DriverResult out = DriverResult::failed(outcome.error);
  if (outcome.failure == OnvifFailure::Unauthorized)
    out.failure = "credential_refused";
  return out;
}

DriverResult answered(Json::Value data)
{
  return {.ok = true, .error = {}, .data = std::move(data)};
}
}

OnvifDriver::OnvifDriver(const CameraSchema& camera)
    : OnvifDriver(camera, std::make_shared<OnvifHttpTransport>(endpointOf(camera)))
{
}

OnvifDriver::OnvifDriver(const CameraSchema& camera, std::shared_ptr<IOnvifTransport> transport)
    : camera_(camera),
      client_({.endpoint = endpointOf(camera), .transport = std::move(transport), .clock = {}})
{
}

OnvifEndpoint OnvifDriver::endpointOf(const CameraSchema& camera)
{
  const auto* entry = camera_catalog::byId(camera_stream_paths::catalogIdOf(camera.config));
  const int port =
      entry != nullptr && entry->defaults.onvifPort > 0 ? entry->defaults.onvifPort : kDefaultPort;
  return {.host = camera.ip,
          .port = port,
          .username = camera.username,
          .password = camera.password,
          .timeoutSeconds = kTimeoutSeconds};
}

Json::Value OnvifDriver::capabilities() const
{
  Json::Value out = camera_capabilities::of(camera_);
  out["holdRenewMs"] = kHoldRenewMs;
  const std::scoped_lock lock(knownMutex_);
  if (known_) {
    out["ptz"] = known_->ptz;
    out["presets"] = known_->presets;
    out["streamOnly"] = !known_->ptz && !known_->presets;
  }
  return out;
}

Json::Value OnvifDriver::discoverCapabilities()
{
  {
    const std::scoped_lock lock(callMutex_);
    static_cast<void>(connected());
  }
  return capabilities();
}

DriverResult OnvifDriver::connected()
{
  if (client_.device())
    return answered(Json::Value());
  const auto outcome = client_.connect();
  if (!outcome.ok())
    return failureOf(outcome);
  {
    const std::scoped_lock lock(knownMutex_);
    known_ = outcome.value;
  }
  if (std::string reported = camera_capabilities::listOf(capabilities()); reported != camera_.capabilities)
    capability_record::learned({.cameraId = camera_.id, .capabilities = std::move(reported)});
  return answered(Json::Value());
}

DriverResult OnvifDriver::status()
{
  const std::scoped_lock lock(callMutex_);
  if (auto ready = connected(); !ready.ok)
    return ready;
  const OnvifDevice& device = client_.device().value_or(OnvifDevice{});
  Json::Value out(Json::objectValue);
  out["model"] = device.info.model.empty() ? camera_.model : device.info.model;
  out["manufacturer"] = device.info.manufacturer;
  out["firmware"] = device.info.firmware;
  out["hardwareVersion"] = device.info.hardwareId;
  out["serial"] = device.info.serial;
  out["clockOffsetSeconds"] = static_cast<Json::Int64>(device.clockOffsetSeconds);
  out["streamOnly"] = !device.ptz && !device.presets;
  return answered(std::move(out));
}

DriverResult OnvifDriver::presets()
{
  const std::scoped_lock lock(callMutex_);
  if (auto ready = connected(); !ready.ok)
    return ready;
  const auto stored = client_.presets();
  if (!stored.ok())
    return failureOf(stored);
  Json::Value ids(Json::arrayValue);
  Json::Value names(Json::arrayValue);
  for (const auto& entry : stored.value) {
    ids.append(entry.token);
    names.append(entry.name);
  }
  Json::Value out(Json::objectValue);
  out["preset"]["id"] = std::move(ids);
  out["preset"]["name"] = std::move(names);
  return answered(std::move(out));
}

DriverResult OnvifDriver::move(const DriverMoveInput& input)
{
  const std::scoped_lock lock(callMutex_);
  if (auto ready = connected(); !ready.ok)
    return ready;
  OnvifOutcome<bool> outcome;
  if (input.stop) {
    outcome = client_.stop();
  }
  else if (input.angle) {
    const double radians = static_cast<double>(*input.angle) * std::numbers::pi / 180.0;
    outcome = client_.continuousMove({.x = kHoldSpeed * std::cos(radians), .y = kHoldSpeed * std::sin(radians)});
  }
  else {
    outcome = client_.relativeMove({.x = static_cast<double>(input.x.value_or(0)) / kPanSpanDegrees,
                                    .y = static_cast<double>(input.y.value_or(0)) / kTiltSpanDegrees});
  }
  if (!outcome.ok())
    return failureOf(outcome);
  Json::Value out(Json::objectValue);
  out["moved"] = true;
  out["limit"] = false;
  return answered(std::move(out));
}

DriverResult OnvifDriver::preset(const DriverPresetInput& input)
{
  const std::scoped_lock lock(callMutex_);
  if (auto ready = connected(); !ready.ok)
    return ready;
  if (input.action == "save") {
    const auto saved = client_.savePreset(input.name);
    if (!saved.ok())
      return failureOf(saved);
    Json::Value out(Json::objectValue);
    out["id"] = saved.value;
    return answered(std::move(out));
  }
  const auto done = input.action == "delete" ? client_.removePreset(input.id) : client_.gotoPreset(input.id);
  if (!done.ok())
    return failureOf(done);
  return answered(Json::Value(Json::objectValue));
}

DriverResult OnvifDriver::settings(const DriverSettingsInput&)
{
  return DriverResult::failed("Argus drives only pan, tilt and presets on ONVIF cameras; this setting is not available");
}

DriverResult OnvifDriver::speak(const DriverSpeakInput&)
{
  return DriverResult::failed("This camera has no speaker Argus can reach");
}
