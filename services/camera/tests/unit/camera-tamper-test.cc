#include <doctest/doctest.h>

#include <feature/monitor/camera-health-monitor.hxx>
#include <feature/monitor/health-event.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <shared/services/stream/frame-source.hxx>
#include <shared/schemas/camera/camera-schema.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace
{
class StillSource final : public IFrameSource
{
public:
  drogon::Task<std::optional<CameraFrame>> grab(const FrameGrabRequest&) override
  {
    co_return frame;
  }

  CameraFrame frame;
};

class EventSink final : public IHealthEventSink
{
public:
  bool publish(const CameraHealthEvent& event) override
  {
    events.push_back(event);
    return true;
  }

  bool publishPrivacy(const CameraPrivacyEvent&) override { return true; }

  std::vector<CameraHealthEvent> events;
};

struct Pattern
{
  uint32_t seed{1};
  bool color{false};
  int shiftX{0};
};

cv::Mat texture(const Pattern& pattern)
{
  cv::Mat image(90, 160, CV_8UC3);
  uint32_t state = pattern.seed;
  for (int row = 0; row < image.rows; row += 10) {
    for (int column = 0; column < image.cols; column += 10) {
      state = state * 1664525U + 1013904223U;
      const double base = 60.0 + static_cast<double>((state >> 24) % 141);
      const cv::Scalar fill = pattern.color ? cv::Scalar(base * 0.5, base, base * 0.8)
                                            : cv::Scalar(base, base, base);
      cv::rectangle(image, cv::Rect(column, row, 10, 10), fill, cv::FILLED);
    }
  }
  if (pattern.shiftX != 0) {
    const cv::Mat shift = (cv::Mat_<double>(2, 3) << 1, 0, pattern.shiftX, 0, 1, 0);
    cv::Mat moved;
    cv::warpAffine(image, moved, shift, image.size(), cv::INTER_NEAREST, cv::BORDER_REPLICATE);
    return moved;
  }
  return image;
}

std::vector<uint8_t> jpegOf(const cv::Mat& image)
{
  std::vector<uint8_t> jpeg;
  REQUIRE(cv::imencode(".jpg", image, jpeg, {cv::IMWRITE_JPEG_QUALITY, 95}));
  return jpeg;
}

HealthThresholds thresholds()
{
  return HealthThresholds{.dark = 25.0,
                          .bright = 235.0,
                          .blur = 18.0,
                          .sceneDiff = 0.35,
                          .saturatedShare = 0.6,
                          .whiteoutEdgeRatio = 0.15,
                          .blindEdgeRatio = 0.5,
                          .coverEntropy = 3.0,
                          .coverEdgeRatio = 0.2,
                          .partialBlockShare = 0.28,
                          .defocusSharpness = 0.4,
                          .reaimShiftPx = 5.0,
                          .infraredChroma = 4.0};
}

HealthMetrics steady()
{
  return HealthMetrics{.brightness = 120.0,
                       .blur = 80.0,
                       .sceneDiff = 0.05,
                       .paused = false,
                       .homeDrift = false,
                       .saturated = 0.0,
                       .edgeRatio = 1.0,
                       .sharpness = 1.0,
                       .blocksLost = 0.0,
                       .shiftPx = 0.0,
                       .entropy = 7.0,
                       .chroma = 10.0,
                       .infrared = false,
                       .sudden = false,
                       .held = false};
}

struct Scene
{
  StillSource source;
  EventSink sink;
  std::vector<TamperOnset> onsets;
  CameraHealthMonitor monitor;
  int64_t cameraId;

  explicit Scene(int64_t id)
      : monitor({.source = &source,
                 .sink = &sink,
                 .presence = nullptr,
                 .active = {},
                 .homeDrift = {},
                 .protects = {},
                 .tamperOnset = [this](TamperOnset onset) { onsets.push_back(std::move(onset)); }},
                {.enabled = true,
                 .intervalMs = 1000,
                 .thresholds = thresholds(),
                 .rebaselineAfterMs = 600000,
                 .protectIntervalMs = 5000,
                 .dayNightHoldMs = 30000}),
        cameraId(id)
  {
  }

  CameraHealthState show(const cv::Mat& image, int64_t atMs)
  {
    source.frame.jpeg = jpegOf(image);
    source.frame.capturedAtMs = atMs;
    drogon::sync_wait(monitor.tick({.id = cameraId, .name = "Patio", .camera = {}}));
    REQUIRE_FALSE(sink.events.empty());
    return sink.events.back().status;
  }
};
}

TEST_CASE("every row of the tamper table classifies from its own feature")
{
  const HealthThresholds limits = thresholds();

  HealthMetrics cover = steady();
  cover.brightness = 8.0;
  cover.blur = 2.0;
  CHECK(health_monitor::classify(cover, limits) == CameraHealthState::Covered);

  HealthMetrics spray = steady();
  spray.entropy = 1.5;
  spray.edgeRatio = 0.05;
  CHECK(health_monitor::classify(spray, limits) == CameraHealthState::Covered);

  HealthMetrics whiteout = steady();
  whiteout.brightness = 250.0;
  whiteout.saturated = 0.9;
  whiteout.edgeRatio = 0.05;
  CHECK(health_monitor::classify(whiteout, limits) == CameraHealthState::Whiteout);

  HealthMetrics blinded = steady();
  blinded.brightness = 210.0;
  blinded.saturated = 0.65;
  blinded.edgeRatio = 0.3;
  CHECK(health_monitor::classify(blinded, limits) == CameraHealthState::Blinded);

  HealthMetrics partial = steady();
  partial.blocksLost = 0.3;
  CHECK(health_monitor::classify(partial, limits) == CameraHealthState::PartialCover);

  HealthMetrics defocus = steady();
  defocus.sharpness = 0.3;
  CHECK(health_monitor::classify(defocus, limits) == CameraHealthState::Blurred);

  HealthMetrics reaimed = steady();
  reaimed.shiftPx = 8.0;
  CHECK(health_monitor::classify(reaimed, limits) == CameraHealthState::Moved);

  HealthMetrics darkSudden = steady();
  darkSudden.brightness = 15.0;
  darkSudden.sudden = true;
  CHECK(health_monitor::classify(darkSudden, limits) == CameraHealthState::DarkSudden);

  HealthMetrics darkSlow = darkSudden;
  darkSlow.sudden = false;
  CHECK(health_monitor::classify(darkSlow, limits) == CameraHealthState::Dark);
}

TEST_CASE("only the tampering states open an episode, and blur only when it is sudden")
{
  CHECK(health_monitor::tampering(CameraHealthState::Covered, false));
  CHECK(health_monitor::tampering(CameraHealthState::Whiteout, false));
  CHECK(health_monitor::tampering(CameraHealthState::Blinded, false));
  CHECK(health_monitor::tampering(CameraHealthState::PartialCover, false));
  CHECK(health_monitor::tampering(CameraHealthState::Moved, false));
  CHECK(health_monitor::tampering(CameraHealthState::DarkSudden, false));
  CHECK(health_monitor::tampering(CameraHealthState::Unreachable, false));
  CHECK(health_monitor::tampering(CameraHealthState::Blurred, true));
  CHECK_FALSE(health_monitor::tampering(CameraHealthState::Blurred, false));
  CHECK_FALSE(health_monitor::tampering(CameraHealthState::Dark, false));
  CHECK_FALSE(health_monitor::tampering(CameraHealthState::Bright, false));
  CHECK_FALSE(health_monitor::tampering(CameraHealthState::Ok, false));
  CHECK(health_monitor::statusName(CameraHealthState::Whiteout) == "whiteout");
  CHECK(health_monitor::statusName(CameraHealthState::Blinded) == "blinded");
  CHECK(health_monitor::statusName(CameraHealthState::PartialCover) == "partial_cover");
  CHECK(health_monitor::statusName(CameraHealthState::DarkSudden) == "dark_sudden");
}

TEST_CASE("a white frame after a textured view is an IR whiteout and keeps the last good frame")
{
  Scene scene(301);
  CHECK(scene.show(texture({.seed = 3, .color = false, .shiftX = 0}), 1000) == CameraHealthState::Ok);
  CHECK(scene.show(texture({.seed = 3, .color = false, .shiftX = 0}), 2000) == CameraHealthState::Ok);
  const cv::Mat white(90, 160, CV_8UC3, cv::Scalar(255, 255, 255));
  CHECK(scene.show(white, 3000) == CameraHealthState::Whiteout);
  REQUIRE(scene.onsets.size() == 1);
  CHECK(scene.onsets.front().status == CameraHealthState::Whiteout);
  CHECK_FALSE(scene.onsets.front().lastGoodJpeg.empty());
  CHECK(scene.onsets.front().lastGoodAtMs == 2000);
  CHECK(scene.show(white, 4000) == CameraHealthState::Whiteout);
  CHECK(scene.onsets.size() == 1);
  CHECK(scene.sink.events.back().onsetAtMs == 3000);
}

TEST_CASE("half the lens covered is a partial cover")
{
  Scene scene(302);
  CHECK(scene.show(texture({.seed = 5, .color = false, .shiftX = 0}), 1000) == CameraHealthState::Ok);
  cv::Mat half = texture({.seed = 5, .color = false, .shiftX = 0});
  cv::rectangle(half, cv::Rect(0, 0, 80, 90), cv::Scalar(128, 128, 128), cv::FILLED);
  CHECK(scene.show(half, 2000) == CameraHealthState::PartialCover);
  CHECK(scene.sink.events.back().metrics.blocksLost >= 0.28);
  CHECK(scene.onsets.size() == 1);
}

TEST_CASE("a sudden defocus is blurred and marked sudden")
{
  Scene scene(303);
  const cv::Mat sharp = texture({.seed = 9, .color = false, .shiftX = 0});
  CHECK(scene.show(sharp, 1000) == CameraHealthState::Ok);
  cv::Mat soft;
  cv::GaussianBlur(sharp, soft, cv::Size(15, 15), 6.0);
  CHECK(scene.show(soft, 2000) == CameraHealthState::Blurred);
  CHECK(scene.sink.events.back().metrics.sudden);
  CHECK(scene.onsets.size() == 1);
}

TEST_CASE("a re-aimed view is moved")
{
  Scene scene(304);
  CHECK(scene.show(texture({.seed = 13, .color = false, .shiftX = 0}), 1000) == CameraHealthState::Ok);
  CHECK(scene.show(texture({.seed = 13, .color = false, .shiftX = 12}), 2000) == CameraHealthState::Moved);
}

TEST_CASE("the day/night switch holds every check and keeps one reference per light")
{
  Scene scene(305);
  CHECK(scene.show(texture({.seed = 17, .color = true, .shiftX = 0}), 1000) == CameraHealthState::Ok);
  CHECK_FALSE(scene.sink.events.back().metrics.infrared);
  CHECK(scene.show(texture({.seed = 18, .color = false, .shiftX = 0}), 2000) == CameraHealthState::Ok);
  CHECK(scene.sink.events.size() == 1);
  CHECK(scene.show(texture({.seed = 18, .color = false, .shiftX = 0}), 40000) == CameraHealthState::Ok);
  CHECK(scene.sink.events.back().metrics.infrared);
  CHECK(scene.onsets.empty());
}

TEST_CASE("no tamper fires while Argus moves the camera or while the lens is masked")
{
  Scene moving(306);
  CHECK(moving.show(texture({.seed = 21, .color = false, .shiftX = 0}), 1000) == CameraHealthState::Ok);
  CameraSceneLog::instance().beginMove({.cameraId = 306, .startMs = 1500, .expectedEndMs = 9000, .returnsHome = true});
  const cv::Mat white(90, 160, CV_8UC3, cv::Scalar(255, 255, 255));
  CHECK(moving.show(white, 2000) == CameraHealthState::Ok);
  CHECK(moving.onsets.empty());
  CameraSceneLog::instance().forget(306);

  Scene masked(307);
  CameraSchema camera;
  camera.id = 307;
  camera.privacyState = CameraPrivacyState::On;
  PrivacyStateService::instance().seed({camera});
  masked.source.frame.jpeg = jpegOf(white);
  masked.source.frame.capturedAtMs = 1000;
  drogon::sync_wait(masked.monitor.tick({.id = 307, .name = "Patio", .camera = {}}));
  CHECK(masked.sink.events.empty());
  CHECK(masked.onsets.empty());
  PrivacyStateService::instance().forget(307);
}
