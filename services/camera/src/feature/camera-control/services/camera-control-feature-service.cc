#include "camera-control-feature-service.hxx"

#include <drogon/utils/Utilities.h>
#include <runtime/blocking-task.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <sync/camera-change-sink.hxx>
#include <shared/services/camera-driver/stream-only-driver.hxx>
#include <shared/services/privacy/privacy-mask.hxx>
#include <shared/services/stream/snapshot-store.hxx>

#include <chrono>
#include <utility>

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

DriverResult stillOf(const CameraSnapshot& picture)
{
  Json::Value out;
  out["image"] = "data:image/jpeg;base64," + drogon::utils::base64Encode(picture.jpeg);
  out["capturedAt"] = static_cast<Json::Int64>(picture.atMs);
  return {.ok = true, .error = {}, .data = out};
}
}

drogon::Task<CameraControlResult> CameraControlFeatureService::onDevice(
    int64_t cameraId,
    const std::function<DriverResult(ICameraDriver&)>& work) const
{
  const auto camera = co_await cameraRepository_.findById(cameraId);
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

drogon::Task<void>
CameraControlFeatureService::announceStatus(int64_t cameraId) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishResource({.scope = ResourceScope::Status,
                                  .ids = {cameraId},
                                  .user = 0,
                                  .client = nullptr});
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::preset(int64_t cameraId, const CameraPresetDto& body) const
{
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.preset({.action = body.action, .id = body.id, .name = body.name});
  });
  if (succeeded(result) && body.action != "save" && body.action != "delete")
    CameraSceneLog::instance().noteAimed(cameraId, nowMs());
  if (succeeded(result) && (body.action == "save" || body.action == "delete"))
    co_await announceStatus(cameraId);
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
  co_await announceStatus(cameraId);
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

drogon::Task<std::optional<std::vector<uint8_t>>>
CameraControlFeatureService::masked(int64_t cameraId, std::vector<uint8_t> jpeg) const
{
  const auto rows = co_await zoneRepository_.findByCamera(cameraId);
  std::vector<OperatorZone> zones;
  for (const auto& row : rows) {
    if (row.isEnabled && row.zoneType == ZoneType::Privacy)
      zones.push_back({.cameraId = row.cameraId,
                       .name = row.name,
                       .kind = row.zoneType,
                       .points = parseZonePoints(row.points)});
  }
  if (!privacy_mask::covers(zones))
    co_return std::make_optional(std::move(jpeg));
  co_return co_await BlockingTask<std::optional<std::vector<uint8_t>>>(
      [jpeg = std::move(jpeg), zones = std::move(zones)]() {
        return privacy_mask::maskedJpeg(jpeg, zones);
      },
      BlockingLane::Heavy);
}

drogon::Task<DriverResult> CameraControlFeatureService::freshStill(FrameGrabRequest request) const
{
  auto frame = co_await frames_.grab(request);
  if (!frame || frame->jpeg.empty())
    co_return DriverResult::failed("The camera sent no picture");
  const auto jpeg = co_await masked(request.cameraId, std::move(frame->jpeg));
  if (!jpeg)
    co_return DriverResult::failed("The picture could not be masked");
  co_return stillOf({.jpeg = std::string(jpeg->begin(), jpeg->end()), .atMs = frame->capturedAtMs});
}

drogon::Task<DriverResult> CameraControlFeatureService::sharedStill(FrameGrabRequest request) const
{
  auto ticket = mainStills_.join(request.cameraId);
  if (!ticket.leads())
    co_return co_await ticket.result();
  const DriverResult result = co_await freshStill(std::move(request));
  ticket.land(result);
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::snapshot(int64_t cameraId, CameraStream stream) const
{
  const auto camera = co_await cameraRepository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;
  if (!camera->isEnabled)
    co_return DriverResult::failed("This camera is disabled");

  FrameGrabRequest request{.cameraId = cameraId, .cameraName = camera->name, .maxAgeMs = 0, .stream = stream};
  if (!camera_stream_role::isWarm(stream))
    co_return co_await sharedStill(std::move(request));
  if (const auto stored = SnapshotStore::instance().frame(cameraId);
      stored && nowMs() - stored->atMs <= kFreshSnapshotMs)
    co_return stillOf(*stored);
  co_return co_await freshStill(std::move(request));
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::sdDays(int64_t cameraId, const CameraSdDaysDto& query) const
{
  co_return co_await onDevice(cameraId, [query](ICameraDriver& driver) {
    return driver.sdDays({.from = query.from, .to = query.to});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::sdDay(int64_t cameraId, const CameraSdDayDto& query) const
{
  co_return co_await onDevice(cameraId,
                              [query](ICameraDriver& driver) { return driver.sdDay({.date = query.date}); });
}
