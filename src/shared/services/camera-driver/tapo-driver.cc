#include "tapo-driver.hxx"

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/tapo/tapo-talk-client.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>

namespace
{
TapoClientConfig controlConfig(const CameraSchema& camera)
{
  TapoClientConfig config;
  config.host = camera.ip;
  // The row keeps the RTSP port; control speaks HTTPS on its own one.
  config.port = static_cast<int>(ConfigService::getInt("tapo.control_port"));
  config.connectTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.connect_timeout_ms"));
  config.requestTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.request_timeout_ms"));
  config.loginAttempts = static_cast<int>(ConfigService::getInt("tapo.login_attempts"));
  config.transport = tapoTransportPreferenceFromString(ConfigService::getString("tapo.transport"));

  if (!camera.username.empty() && !camera.password.empty())
    config.candidates.push_back({.label = "camera_account",
                                 .username = camera.username,
                                 .password = camera.password});
  // The cloud account is the fallback the probe uses when the camera account is
  // refused, and the only one the talk channel accepts.
  if (!camera.cloudPassword.empty())
    config.candidates.push_back(
        {.label = "cloud_admin",
         .username = camera.cloudUsername.empty() ? "admin" : camera.cloudUsername,
         .password = camera.cloudPassword});
  return config;
}

DriverResult toDriverResult(const TapoResult& result)
{
  return {.ok = result.ok, .error = result.error, .data = result.data};
}
} // namespace

TapoDriver::TapoDriver(const CameraSchema& camera)
    : camera_(camera), api_(std::make_unique<TapoApi>(controlConfig(camera)))
{
}

Json::Value TapoDriver::capabilities() const
{
  Json::Value out;
  out["ptz"] = true;
  out["presets"] = true;
  out["talk"] = !camera_.cloudPassword.empty();
  out["privacy"] = true;
  out["led"] = true;
  out["dayNight"] = true;
  out["motion"] = true;
  out["autoTrack"] = true;
  out["alarm"] = true;
  return out;
}

DriverResult TapoDriver::ensureConnected()
{
  if (api_->isConnected())
    return {.ok = true, .error = {}, .data = Json::Value()};
  return toDriverResult(api_->connect());
}

DriverResult TapoDriver::status()
{
  if (const auto ready = ensureConnected(); !ready.ok)
    return ready;
  const auto batch = api_->getStatus();
  return {.ok = batch.ok, .error = batch.error, .data = batch.toJson()};
}

DriverResult TapoDriver::presets()
{
  if (const auto ready = ensureConnected(); !ready.ok)
    return ready;
  return toDriverResult(api_->getPresets());
}

DriverResult TapoDriver::move(const DriverMoveInput& input)
{
  if (const auto ready = ensureConnected(); !ready.ok)
    return ready;
  if (input.angle)
    return toDriverResult(api_->step({.direction = *input.angle}));
  return toDriverResult(
      api_->move({.x = input.x.value_or(0), .y = input.y.value_or(0)}));
}

DriverResult TapoDriver::preset(const DriverPresetInput& input)
{
  if (const auto ready = ensureConnected(); !ready.ok)
    return ready;
  const TapoPresetInput target{.id = input.id, .name = input.name};
  if (input.action == "save")
    return toDriverResult(api_->savePreset(target));
  if (input.action == "delete")
    return toDriverResult(api_->deletePreset(target));
  return toDriverResult(api_->gotoPreset(target));
}

DriverResult TapoDriver::settings(const DriverSettingsInput& input)
{
  if (const auto ready = ensureConnected(); !ready.ok)
    return ready;

  DriverResult last{.ok = true, .error = {}, .data = Json::Value()};
  const auto apply = [&last](const TapoResult& step) {
    if (last.ok && !step.ok)
      last = toDriverResult(step);
  };

  if (input.privacy)
    apply(api_->setPrivacy({.enabled = *input.privacy}));
  if (input.led)
    apply(api_->setLed({.enabled = *input.led}));
  if (input.dayNight)
    apply(api_->setDayNight({.mode = *input.dayNight}));
  if (input.motion || input.motionSensitivity)
    apply(api_->setMotion(
        {.enabled = input.motion.value_or(true), .sensitivity = input.motionSensitivity}));
  if (input.autoTrack)
    apply(api_->setAutoTrack({.enabled = *input.autoTrack}));
  if (input.alarmVolume)
    apply(api_->setAlarmVolume(*input.alarmVolume <= 33   ? "low"
                               : *input.alarmVolume <= 66 ? "normal"
                                                          : "high"));
  if (input.alarm)
    apply(api_->setAlarm({.enabled = *input.alarm}));

  if (last.ok)
    last.data = api_->getStatus().toJson();
  return last;
}

DriverResult TapoDriver::speak(const DriverSpeakInput& input)
{
  if (camera_.cloudPassword.empty())
    return DriverResult::failure("The talk channel needs the vendor cloud password");
  if (input.samples.empty())
    return DriverResult::failure("Nothing to say");

  TapoTalkConfig config;
  config.host = camera_.ip;
  config.port = static_cast<int>(ConfigService::getInt("tapo.media_port"));
  config.username = camera_.cloudUsername.empty() ? "admin" : camera_.cloudUsername;
  config.cloudPassword = camera_.cloudPassword;
  config.mode = ConfigService::getString("tapo.talk_mode");
  config.framing = tapoTalkFramingFromString(ConfigService::getString("tapo.talk_framing"));
  config.connectTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.connect_timeout_ms"));
  config.ioTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.request_timeout_ms"));

  TapoTalkClient client(config);
  CancellationToken token;
  const auto sent = client.sendChunk(
      {.samples = input.samples, .sampleRate = input.sampleRate, .reopenOnFailure = true},
      token);
  if (!sent.ok)
    return DriverResult::failure(sent.error.empty() ? "The camera refused the audio"
                                                    : sent.error);

  Json::Value data;
  data["spokenSamples"] = static_cast<Json::Int64>(input.samples.size());
  return {.ok = true, .error = {}, .data = data};
}
