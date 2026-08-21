#include "camera-control-feature-service.hxx"

#include <shared/services/tts/tts-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

drogon::Task<CameraControlResult> CameraControlFeatureService::onDevice(
    int64_t cameraId,
    const std::function<DriverResult(ICameraDriver&)>& work) const
{
  const auto camera = co_await repository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;

  auto driver = CameraDriverRegistry::instance().driverFor(*camera);
  if (!driver)
    co_return DriverResult::failure("This camera driver is not supported yet");

  const auto result =
      co_await BlockingTask<DriverResult>([driver, work]() { return work(*driver); });

  // A failed session is dropped so the next call logs in again instead of
  // reusing a transport the camera has already closed.
  if (!result.ok)
    CameraDriverRegistry::instance().forget(cameraId);
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::status(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId,
                              [](ICameraDriver& driver) { return driver.status(); });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::capabilities(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId, [](ICameraDriver& driver) {
    return DriverResult{.ok = true, .error = {}, .data = driver.capabilities()};
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::presets(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId,
                              [](ICameraDriver& driver) { return driver.presets(); });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::move(int64_t cameraId, const CameraPtzDto& body) const
{
  co_return co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.move({.x = body.x, .y = body.y, .angle = body.angle});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::preset(int64_t cameraId, const CameraPresetDto& body) const
{
  co_return co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.preset({.action = body.action, .id = body.id, .name = body.name});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::settings(int64_t cameraId, const CameraSettingsDto& body) const
{
  co_return co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.settings({.privacy = body.privacy,
                            .led = body.led,
                            .dayNight = body.dayNight,
                            .motion = body.motion,
                            .motionSensitivity = body.motionSensitivity,
                            .autoTrack = body.autoTrack,
                            .alarm = body.alarm,
                            .alarmVolume = body.alarmVolume});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::speak(int64_t cameraId, const CameraTalkDto& body) const
{
  // Synthesis is blocking too, and it happens before the device call so a TTS
  // failure never opens a talk session for nothing.
  const auto pcm = co_await BlockingTask<std::vector<int16_t>>([body]() {
    TtsRequest request;
    request.text = body.text;
    request.lang = body.lang == "en" ? TtsLang::EN : TtsLang::ES;
    request.quality = TtsQuality::Auto;
    request.speed = TtsService::instance().defaultSpeed();

    const auto floats = TtsService::instance().synthesize(request);
    std::vector<int16_t> samples;
    samples.reserve(floats.size());
    for (const auto sample : floats) {
      const float clamped = std::max(-1.0F, std::min(1.0F, sample));
      samples.push_back(static_cast<int16_t>(clamped * 32767.0F));
    }
    return samples;
  });

  const int rate = TtsService::instance().sampleRate();
  co_return co_await onDevice(cameraId, [pcm, rate](ICameraDriver& driver) {
    return driver.speak({.samples = pcm, .sampleRate = rate});
  });
}
