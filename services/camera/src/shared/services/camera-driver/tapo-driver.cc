#include "tapo-driver.hxx"

#include <chrono>
#include <config/config-service.hxx>
#include <shared/services/camera-driver/camera-capabilities.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>
#include <shared/services/tapo/tapo-talk-client.hxx>
#include <runtime/cancellation-token.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr int kSpeakWaitSeconds = 3;

TapoClientConfig controlConfig(const CameraSchema& camera)
{
  TapoClientConfig config;
  config.host = camera.ip;
  config.port = static_cast<int>(ConfigService::getInt("tapo.control_port"));
  config.connectTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.connect_timeout_ms"));
  config.requestTimeoutMs = static_cast<int>(ConfigService::getInt("tapo.request_timeout_ms"));
  config.loginAttempts = static_cast<int>(ConfigService::getInt("tapo.login_attempts"));
  config.transport = tapoTransportPreferenceFromString(ConfigService::getString("tapo.transport"));

  if (!camera.username.empty() && !camera.password.empty())
    config.candidates.push_back({.label = "camera_account",
                                 .username = camera.username,
                                 .password = camera.password});
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

std::string frameRateList(const std::vector<int>& rates)
{
  std::string out;
  for (const int rate : rates) {
    if (!out.empty())
      out += ", ";
    out += std::to_string(rate);
  }
  return out;
}

TapoResult applyFrameRate(TapoApi& api, int frameRate)
{
  auto offered = api.getVideoCapability();
  if (!offered.ok)
    return offered;
  const auto code = tapo_video::frameRateCodeFor(offered.data, frameRate);
  if (!code) {
    const Json::Value empty;
    const auto profile = tapo_video::profileOf({.capability = offered.data, .quality = empty});
    const auto rates = profile ? frameRateList(profile->frameRates) : std::string();
    return TapoResult::failure(rates.empty()
                                   ? "This camera does not let Argus change its frame rate"
                                   : "This camera streams at " + rates + " fps only");
  }
  return api.setVideoFrameRate(*code);
}
}

DriverResult TapoDriver::probe(const CameraSchema& camera)
{
  TapoApi api(controlConfig(camera));
  const auto connected = api.connect();
  if (!connected.ok) {
    const bool silent = connected.error.find("no answer") != std::string::npos ||
                        connected.error.find("refused") != std::string::npos ||
                        connected.error.find("timeout") != std::string::npos ||
                        connected.error.find("transport error") != std::string::npos;
    DriverResult failed = DriverResult::failure(connected.error);
    failed.data["reason"] = silent ? "unreachable" : "auth_failed";
    return failed;
  }
  const auto info = api.getDeviceInfo();
  const Json::Value& responses = info.data["result"]["responses"];
  const Json::Value& basic = responses.isArray() && !responses.empty()
                                 ? responses[0]["result"]["device_info"]["basic_info"]
                                 : Json::Value::nullSingleton();
  Json::Value out(Json::objectValue);
  out["model"] = basic.get("device_model", "").asString();
  out["firmware"] = basic.get("sw_version", "").asString();
  return {.ok = true, .error = {}, .data = out};
}

TapoDriver::TapoDriver(const CameraSchema& camera)
    : camera_(camera),
      api_(std::make_unique<TapoApi>(controlConfig(camera))),
      lineMutex_(std::make_shared<std::timed_mutex>())
{
}

Json::Value TapoDriver::capabilities() const
{
  return camera_capabilities::of(camera_);
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
  if (input.stop)
    return toDriverResult(api_->stopMotor());
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
  if (input.frameRate)
    apply(applyFrameRate(*api_, *input.frameRate));
  if (input.sounding) {
    const auto sounded = api_->manualAlarm(*input.sounding);
    if (!sounded.ok)
      LOG_WARN << "Tapo driver: manual alarm " << (*input.sounding ? "start" : "stop")
               << " refused: " << sounded.error;
  }

  if (last.ok)
    last.data = api_->getStatus().toJson();
  return last;
}

TapoDriver::~TapoDriver() = default;

std::string tapoTalkUsername()
{
  return "admin";
}

TapoTalkConfig talkConfigOf(const CameraSchema& camera)
{
  TapoTalkConfig config;
  config.host = camera.ip;
  config.port = static_cast<int>(ConfigService::getInt("tapo.media_port"));
  config.username = tapoTalkUsername();
  config.cloudPassword = camera.cloudPassword;
  config.mode = ConfigService::getString("tapo.talk_mode");
  config.framing =
      tapoTalkFramingFromString(ConfigService::getString("tapo.talk_framing"));
  config.connectTimeoutMs =
      static_cast<int>(ConfigService::getInt("tapo.connect_timeout_ms"));
  config.ioTimeoutMs =
      static_cast<int>(ConfigService::getInt("tapo.request_timeout_ms"));
  return config;
}

DriverResult TapoDriver::speak(const DriverSpeakInput& input)
{
  if (camera_.cloudPassword.empty())
    return DriverResult::failure("The talk channel needs the vendor cloud password");
  if (input.samples.empty())
    return DriverResult::failure("Nothing to say");

  std::unique_lock lock(*lineMutex_, std::chrono::seconds(kSpeakWaitSeconds));
  if (!lock.owns_lock())
    return DriverResult::failure("Someone is talking through this camera right now");

  if (!talkClient_)
    talkClient_ = std::make_shared<TapoTalkClient>(talkConfigOf(camera_));

  CancellationToken token;
  const auto sent = talkClient_->sendChunk(
      {.samples = input.samples, .sampleRate = input.sampleRate, .reopenOnFailure = false},
      token);
  if (!sent.ok) {
    talkClient_.reset();
    return DriverResult::failure(sent.error.empty() ? "The camera refused the audio"
                                                    : sent.error);
  }

  Json::Value data;
  data["spokenSamples"] = static_cast<Json::Int64>(input.samples.size());
  return {.ok = true, .error = {}, .data = data};
}

namespace
{
class TapoTalkLine final : public ICameraTalkLine
{
public:
  TapoTalkLine(std::shared_ptr<std::timed_mutex> mutex,
               std::shared_ptr<TapoTalkClient> client)
      : mutex_(std::move(mutex)), lock_(*mutex_, std::adopt_lock), client_(std::move(client))
  {
  }

  TapoTalkLine(const TapoTalkLine&) = delete;
  TapoTalkLine& operator=(const TapoTalkLine&) = delete;

  ~TapoTalkLine() override { close(); }

  DriverResult open() override
  {
    if (client_->isOpen())
      return {.ok = true, .error = {}, .data = Json::Value()};
    const auto opened = client_->open();
    if (!opened.ok)
      return DriverResult::failure(opened.error.empty() ? "The camera refused the talk session"
                                                        : opened.error);
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  DriverResult write(std::span<const int16_t> pcm8k) override
  {
    const auto sent = client_->sendPacket(encoder_.encode(pcm8k));
    if (!sent.ok)
      return DriverResult::failure(sent.error);
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  void close() override
  {
    if (closed_)
      return;
    closed_ = true;
    client_->close();
  }

private:
  std::shared_ptr<std::timed_mutex> mutex_;
  std::unique_lock<std::timed_mutex> lock_;
  std::shared_ptr<TapoTalkClient> client_;
  TapoVoiceEncoder encoder_;
  bool closed_{false};
};
}

TalkLineOpen TapoDriver::talkLine()
{
  if (camera_.cloudPassword.empty())
    return {.line = nullptr, .error = "The talk channel needs the vendor cloud password"};
  if (!lineMutex_->try_lock_for(std::chrono::seconds(kSpeakWaitSeconds)))
    return {.line = nullptr, .error = "Someone is talking through this camera right now"};
  if (!talkClient_)
    talkClient_ = std::make_shared<TapoTalkClient>(talkConfigOf(camera_));
  return {.line = std::make_unique<TapoTalkLine>(lineMutex_, talkClient_), .error = {}};
}
