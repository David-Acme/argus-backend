#include "camera-control-feature-service.hxx"

#include <drogon/utils/Utilities.h>
#include <runtime/blocking-task.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/camera-driver/stream-only-driver.hxx>
#include <shared/services/stream/snapshot-store.hxx>

#include <chrono>

namespace
{
constexpr int64_t kFreshSnapshotMs = 10000;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool succeeded(const CameraControlResult& result)
{
  return result && result->ok;
}
}

drogon::Task<CameraControlResult> CameraControlFeatureService::onDevice(
    int64_t cameraId,
    const std::function<DriverResult(ICameraDriver&)>& work) const
{
  const auto camera = co_await repository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;

  std::shared_ptr<ICameraDriver> driver =
      CameraDriverRegistry::instance().driverFor(*camera);
  if (!driver)
    driver = std::make_shared<StreamOnlyDriver>(*camera);

  DriverResult result =
      co_await BlockingTask<DriverResult>([driver, work]() { return work(*driver); });
  result.attempted = true;
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
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.move({.x = body.x, .y = body.y, .angle = body.angle, .stop = body.stop});
  });
  if (succeeded(result))
    CameraSceneLog::instance().noteAimed(cameraId, nowMs());
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::preset(int64_t cameraId, const CameraPresetDto& body) const
{
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.preset({.action = body.action, .id = body.id, .name = body.name});
  });
  if (succeeded(result) && body.action != "save" && body.action != "delete")
    CameraSceneLog::instance().noteAimed(cameraId, nowMs());
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::settings(int64_t cameraId, const CameraSettingsDto& body) const
{
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.settings({.privacy = body.privacy,
                            .led = body.led,
                            .dayNight = body.dayNight,
                            .motion = body.motion,
                            .motionSensitivity = body.motionSensitivity,
                            .autoTrack = body.autoTrack,
                            .alarm = body.alarm,
                            .alarmVolume = body.alarmVolume,
                            .sounding = std::nullopt});
  });
  if (!succeeded(result))
    co_return result;
  auto& scenes = CameraSceneLog::instance();
  if (body.privacy)
    scenes.notePrivacy(cameraId, *body.privacy);
  if ((body.privacy && !*body.privacy) || body.dayNight)
    scenes.noteAimed(cameraId, nowMs());
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::speak(int64_t cameraId, const CameraTalkDto& body) const
{
  std::pair<std::vector<int16_t>, int> audio;
  try {
    audio = co_await BlockingTask<std::pair<std::vector<int16_t>, int>>(
        [this, body]() {
          TtsRequest request;
          request.text = body.text;
          request.lang = body.lang == "en" ? TtsLang::EN : TtsLang::ES;
          request.quality = TtsQuality::Auto;
          request.speed = tts_.defaultSpeed();

          const auto floats = tts_.synthesize(request);
          const int rate = tts_.sampleRate();
          std::vector<int16_t> samples;
          samples.reserve(floats.size());
          for (const auto sample : floats) {
            const float clamped = std::max(-1.0F, std::min(1.0F, sample));
            samples.push_back(static_cast<int16_t>(clamped * 32767.0F));
          }
          return std::make_pair(std::move(samples), rate);
        });
  }
  catch (const std::exception& e) {
    co_return DriverResult::failed("Text-to-speech unavailable: " +
                                    std::string(e.what()));
  }

  const auto& [pcm, rate] = audio;
  co_return co_await onDevice(cameraId, [pcm, rate](ICameraDriver& driver) {
    return driver.speak({.samples = pcm, .sampleRate = rate});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::snapshot(int64_t cameraId) const
{
  const auto camera = co_await repository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;
  if (!camera->isEnabled)
    co_return DriverResult::failed("This camera is disabled");

  CameraSnapshot picture;
  if (auto stored = SnapshotStore::instance().frame(cameraId);
      stored && nowMs() - stored->atMs <= kFreshSnapshotMs) {
    picture = std::move(*stored);
  }
  else {
    const auto frame =
        co_await frames_.grab({.cameraId = cameraId, .cameraName = camera->name, .maxAgeMs = 0});
    if (!frame || frame->jpeg.empty())
      co_return DriverResult::failed("The camera sent no picture");
    picture = {.jpeg = std::string(frame->jpeg.begin(), frame->jpeg.end()),
               .atMs = frame->capturedAtMs};
  }

  Json::Value out;
  out["image"] = "data:image/jpeg;base64," + drogon::utils::base64Encode(picture.jpeg);
  out["capturedAt"] = static_cast<Json::Int64>(picture.atMs);
  co_return DriverResult{.ok = true, .error = {}, .data = out};
}
