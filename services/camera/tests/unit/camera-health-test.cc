#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/monitor/health-event.hxx>
#include <feature/monitor/camera-health-monitor.hxx>
#include <shared/services/stream/frame-source.hxx>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/camera-driver/camera-scene-log.hxx>

#include <cstdint>
#include <vector>

class SyntheticHealthSource final : public IFrameSource
{
public:
  SyntheticHealthSource()
  {
    cv::Mat image(90, 160, CV_8UC3);
    for (int row = 0; row < image.rows; ++row) {
      for (int column = 0; column < image.cols; ++column) {
        const uint8_t value = (row + column) % 2 == 0 ? 80 : 160;
        image.at<cv::Vec3b>(row, column) = cv::Vec3b(value, value, value);
      }
    }
    REQUIRE(cv::imencode(".jpg", image, frame.jpeg));
    frame.capturedAtMs = 1000;
  }

  drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest&) override
  {
    ++calls;
    if (!answering)
      co_return std::nullopt;
    co_return frame;
  }

  CameraFrame frame;
  int calls{0};
  bool answering{true};
};

class RecordingHealthSink final : public IHealthEventSink
{
public:
  bool publish(const CameraHealthEvent& event) override
  {
    events.push_back(event);
    return true;
  }

  std::vector<CameraHealthEvent> events;
};

TEST_CASE("Round15 healthy first tick and periodic ticks publish")
{
  SyntheticHealthSource source;
  RecordingHealthSink sink;
  CameraHealthMonitor monitor({.source = &source, .sink = &sink},
                              {.enabled = true, .intervalMs = 1000,
                               .thresholds = {},
                               .rebaselineAfterMs = 900000});
  int64_t firstStamp = 1000;
  SUBCASE("positive first timestamp") { firstStamp = 1000; }
  SUBCASE("zero first timestamp") { firstStamp = 0; }
  source.frame.capturedAtMs = firstStamp;
  drogon::sync_wait(monitor.tick({.id = 1, .name = "Synthetic"}));
  CHECK(sink.events.size() == 1);
  source.frame.capturedAtMs = firstStamp + 999;
  drogon::sync_wait(monitor.tick({.id = 1, .name = "Synthetic"}));
  CHECK(sink.events.size() == 1);
  source.frame.capturedAtMs = firstStamp + 1000;
  drogon::sync_wait(monitor.tick({.id = 1, .name = "Synthetic"}));
  CHECK(sink.events.size() == 2);
  source.frame.capturedAtMs = firstStamp + 2000;
  drogon::sync_wait(monitor.tick({.id = 1, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 3);
  CHECK(source.calls == 4);
  for (const auto& event : sink.events) {
    CHECK(event.cameraId == 1);
    CHECK(event.cameraName == "Synthetic");
    CHECK(event.status == CameraHealthState::Ok);
    CHECK(event.metrics.blur > 18.0);
  }
  CHECK(sink.events[0].detectedAtMs == firstStamp);
  CHECK(sink.events[1].detectedAtMs == firstStamp + 1000);
  CHECK(sink.events[2].detectedAtMs == firstStamp + 2000);
}


TEST_CASE("one missed sample keeps the health state and two make the camera unreachable")
{
  SyntheticHealthSource source;
  RecordingHealthSink sink;
  CameraHealthMonitor monitor({.source = &source, .sink = &sink},
                              {.enabled = true,
                               .intervalMs = 1000,
                               .thresholds = {},
                               .rebaselineAfterMs = 900000});
  drogon::sync_wait(monitor.tick({.id = 7, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events.back().status == CameraHealthState::Ok);

  source.answering = false;
  source.frame.capturedAtMs = 2000;
  drogon::sync_wait(monitor.tick({.id = 7, .name = "Synthetic"}));
  CHECK(sink.events.size() == 1);

  source.frame.capturedAtMs = 3000;
  drogon::sync_wait(monitor.tick({.id = 7, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 2);
  CHECK(sink.events.back().status == CameraHealthState::Unreachable);

  source.answering = true;
  source.frame.capturedAtMs = 4000;
  drogon::sync_wait(monitor.tick({.id = 7, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 3);
  CHECK(sink.events.back().status == CameraHealthState::Ok);
  CHECK(source.calls == 4);
}

TEST_CASE("a frame that does not decode is a miss, not a first strike")
{
  SyntheticHealthSource source;
  RecordingHealthSink sink;
  CameraHealthMonitor monitor({.source = &source, .sink = &sink},
                              {.enabled = true,
                               .intervalMs = 1000,
                               .thresholds = {},
                               .rebaselineAfterMs = 900000});
  drogon::sync_wait(monitor.tick({.id = 8, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events.back().status == CameraHealthState::Ok);

  source.frame.jpeg = {0x01, 0x02, 0x03};
  source.frame.capturedAtMs = 2000;
  drogon::sync_wait(monitor.tick({.id = 8, .name = "Synthetic"}));
  CHECK(sink.events.size() == 1);

  source.answering = false;
  source.frame.capturedAtMs = 3000;
  drogon::sync_wait(monitor.tick({.id = 8, .name = "Synthetic"}));
  REQUIRE(sink.events.size() == 2);
  CHECK(sink.events.back().status == CameraHealthState::Unreachable);
}

namespace
{
HealthThresholds thresholds()
{
  return HealthThresholds{.dark = 25.0,
                          .bright = 235.0,
                          .blur = 18.0,
                          .sceneDiff = 0.35};
}

HealthMetrics healthy()
{
  return HealthMetrics{.brightness = 120.0, .blur = 80.0, .sceneDiff = 0.05};
}
}

TEST_CASE("a healthy image is ok")
{
  CHECK(health_monitor::classify(healthy(), thresholds()) == CameraHealthState::Ok);
}

TEST_CASE("a dark image is occluded")
{
  auto metrics = healthy();
  metrics.brightness = 10.0;
  CHECK(health_monitor::classify(metrics, thresholds()) == CameraHealthState::Dark);
}

TEST_CASE("a saturated image is bright")
{
  auto metrics = healthy();
  metrics.brightness = 250.0;
  CHECK(health_monitor::classify(metrics, thresholds()) == CameraHealthState::Bright);
}

TEST_CASE("a flat image is blurred")
{
  auto metrics = healthy();
  metrics.blur = 4.0;
  CHECK(health_monitor::classify(metrics, thresholds()) == CameraHealthState::Blurred);
}

TEST_CASE("a large scene change is a moved camera")
{
  auto metrics = healthy();
  metrics.sceneDiff = 0.7;
  CHECK(health_monitor::classify(metrics, thresholds()) == CameraHealthState::Moved);
}

TEST_CASE("darkness wins over blur")
{
  auto metrics = healthy();
  metrics.brightness = 3.0;
  metrics.blur = 1.0;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Covered);
}

TEST_CASE("a dark but textured image stays dark")
{
  auto metrics = healthy();
  metrics.brightness = 10.0;
  metrics.blur = 80.0;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Dark);
}

TEST_CASE("a sharp static frame is ok")
{
  auto metrics = healthy();
  metrics.brightness = 120.0;
  metrics.blur = 850.0;
  metrics.sceneDiff = 0.04;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Ok);
}

TEST_CASE("a sharp night frame with IR light is ok")
{
  auto metrics = healthy();
  metrics.brightness = 90.0;
  metrics.blur = 400.0;
  metrics.sceneDiff = 0.06;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Ok);
}

TEST_CASE("ordinary motion below the move threshold is ok")
{
  auto metrics = healthy();
  metrics.brightness = 120.0;
  metrics.blur = 700.0;
  metrics.sceneDiff = 0.2;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Ok);
}

TEST_CASE("a defocused frame is blurred at any brightness")
{
  auto metrics = healthy();
  metrics.brightness = 120.0;
  metrics.blur = 8.0;
  metrics.sceneDiff = 0.05;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Blurred);
}

TEST_CASE("a dark featureless frame is covered")
{
  auto metrics = healthy();
  metrics.brightness = 8.0;
  metrics.blur = 4.0;
  metrics.sceneDiff = 0.0;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Covered);
}

TEST_CASE("a large sharp scene change is a moved camera")
{
  auto metrics = healthy();
  metrics.blur = 600.0;
  metrics.sceneDiff = 0.7;
  CHECK(health_monitor::classify(metrics, thresholds()) ==
        CameraHealthState::Moved);
}

TEST_CASE("status names are stable")
{
  CHECK(health_monitor::statusName(CameraHealthState::Ok) == "ok");
  CHECK(health_monitor::statusName(CameraHealthState::Moved) == "moved");
  CHECK(health_monitor::statusName(CameraHealthState::Unreachable) == "unreachable");
  CHECK(health_monitor::statusName(CameraHealthState::Covered) == "covered");
}

TEST_CASE("the health monitor reports drained while idle and stops on request")
{
  SyntheticHealthSource source;
  RecordingHealthSink sink;
  CameraHealthMonitor monitor({.source = &source, .sink = &sink},
                              {.enabled = true,
                               .intervalMs = 1000,
                               .thresholds = {},
                               .rebaselineAfterMs = 900000});
  CHECK(monitor.drained());
  CHECK_FALSE(monitor.running());

  monitor.requestStop();
  CHECK_FALSE(monitor.running());
  CHECK(monitor.drained());

  monitor.requestStop();
  CHECK_FALSE(monitor.running());
  CHECK(monitor.drained());
}

namespace
{
struct Texture
{
  uint32_t seed{0};
  double gain{1.0};
  double offset{0.0};
};

std::vector<uint8_t> texturedJpeg(const Texture& texture)
{
  const uint32_t seed = texture.seed;
  const double gain = texture.gain;
  const double offset = texture.offset;
  cv::Mat image(90, 160, CV_8UC3);
  uint32_t state = seed;
  for (int row = 0; row < image.rows; row += 10) {
    for (int column = 0; column < image.cols; column += 10) {
      state = state * 1664525U + 1013904223U;
      const double base = 60.0 + static_cast<double>((state >> 24) % 141);
      const auto value = static_cast<uint8_t>(base * gain + offset);
      cv::rectangle(image, cv::Rect(column, row, 10, 10),
                    cv::Scalar(value, value, value), cv::FILLED);
    }
  }
  std::vector<uint8_t> jpeg;
  REQUIRE(cv::imencode(".jpg", image, jpeg));
  return jpeg;
}

struct SceneSetup
{
  int64_t cameraId{0};
  int64_t rebaselineAfterMs{0};
};

struct SceneRun
{
  SyntheticHealthSource source;
  RecordingHealthSink sink;
  CameraHealthMonitor monitor;
  int64_t cameraId;

  explicit SceneRun(const SceneSetup& setup)
      : monitor({.source = &source, .sink = &sink},
                {.enabled = true,
                 .intervalMs = 1000,
                 .thresholds = thresholds(),
                 .rebaselineAfterMs = setup.rebaselineAfterMs}),
        cameraId(setup.cameraId)
  {
  }

  CameraHealthState show(const std::vector<uint8_t>& jpeg, int64_t atMs)
  {
    source.frame.jpeg = jpeg;
    source.frame.capturedAtMs = atMs;
    drogon::sync_wait(monitor.tick({.id = cameraId, .name = "Scene"}));
    REQUIRE_FALSE(sink.events.empty());
    return sink.events.back().status;
  }
};
}

TEST_CASE("a re-aimed camera is moved and a dimmer room is not")
{
  const auto room = texturedJpeg({.seed = 7, .gain = 1.0, .offset = 0.0});
  SceneRun run({.cameraId = 101, .rebaselineAfterMs = 900000});
  CHECK(run.show(room, 1000) == CameraHealthState::Ok);
  CHECK(run.show(texturedJpeg({.seed = 7, .gain = 0.6, .offset = 20.0}), 2000) == CameraHealthState::Ok);
  CHECK(run.sink.events.back().metrics.sceneDiff < 0.1);
  CHECK(run.show(texturedJpeg({.seed = 99, .gain = 1.0, .offset = 0.0}), 3000) == CameraHealthState::Moved);
  CHECK(run.sink.events.back().metrics.sceneDiff > 0.5);
}

TEST_CASE("a new view that holds past the window becomes the reference")
{
  const auto before = texturedJpeg({.seed = 11, .gain = 1.0, .offset = 0.0});
  const auto after = texturedJpeg({.seed = 12, .gain = 1.0, .offset = 0.0});
  SceneRun run({.cameraId = 102, .rebaselineAfterMs = 5000});
  CHECK(run.show(before, 1000) == CameraHealthState::Ok);
  CHECK(run.show(after, 2000) == CameraHealthState::Moved);
  CHECK(run.show(after, 6000) == CameraHealthState::Moved);
  CHECK(run.show(texturedJpeg({.seed = 13, .gain = 1.0, .offset = 0.0}), 6500) == CameraHealthState::Moved);
  CHECK(run.show(texturedJpeg({.seed = 13, .gain = 1.0, .offset = 0.0}), 11000) == CameraHealthState::Moved);
  CHECK(run.show(texturedJpeg({.seed = 13, .gain = 1.0, .offset = 0.0}), 11500) == CameraHealthState::Ok);
  CHECK(run.show(before, 12500) == CameraHealthState::Moved);
}

TEST_CASE("a view Argus aimed is the reference at once")
{
  SceneRun run({.cameraId = 103, .rebaselineAfterMs = 900000});
  CHECK(run.show(texturedJpeg({.seed = 21, .gain = 1.0, .offset = 0.0}), 1000) == CameraHealthState::Ok);
  CameraSceneLog::instance().noteAimed(103, 1500);
  CHECK(run.show(texturedJpeg({.seed = 22, .gain = 1.0, .offset = 0.0}), 2000) == CameraHealthState::Ok);
  CHECK(run.show(texturedJpeg({.seed = 21, .gain = 1.0, .offset = 0.0}), 4000) == CameraHealthState::Moved);
  CameraSceneLog::instance().forget(103);
}

TEST_CASE("a camera in privacy mode is not checked")
{
  SceneRun run({.cameraId = 104, .rebaselineAfterMs = 900000});
  CameraSceneLog::instance().notePrivacy(104, true);
  run.source.frame.capturedAtMs = 1000;
  drogon::sync_wait(run.monitor.tick({.id = 104, .name = "Scene"}));
  CHECK(run.source.calls == 0);
  CHECK(run.sink.events.empty());
  CameraSceneLog::instance().notePrivacy(104, false);
  drogon::sync_wait(run.monitor.tick({.id = 104, .name = "Scene"}));
  CHECK(run.source.calls == 1);
  CameraSceneLog::instance().forget(104);
}

TEST_CASE("a preset that settles between samples is not moved")
{
  const auto before = texturedJpeg({.seed = 41, .gain = 1.0, .offset = 0.0});
  const auto moving = texturedJpeg({.seed = 42, .gain = 1.0, .offset = 0.0});
  const auto settled = texturedJpeg({.seed = 43, .gain = 1.0, .offset = 0.0});
  SceneRun run({.cameraId = 105, .rebaselineAfterMs = 900000});
  CHECK(run.show(before, 1000) == CameraHealthState::Ok);
  CameraSceneLog::instance().noteAimed(105, 1400);

  run.source.frame.jpeg = moving;
  run.source.frame.capturedAtMs = 1500;
  drogon::sync_wait(run.monitor.tick({.id = 105, .name = "Scene"}));

  run.source.frame.jpeg = settled;
  run.source.frame.capturedAtMs = 2400;
  drogon::sync_wait(run.monitor.tick({.id = 105, .name = "Scene"}));
  REQUIRE(run.sink.events.size() == 2);
  CHECK(run.sink.events.back().status == CameraHealthState::Ok);

  CHECK(run.show(settled, 4000) == CameraHealthState::Ok);
  CHECK(run.show(moving, 5000) == CameraHealthState::Moved);
  CameraSceneLog::instance().forget(105);
}

TEST_CASE("a view nothing aimed still reads moved at the same samples")
{
  const auto before = texturedJpeg({.seed = 51, .gain = 1.0, .offset = 0.0});
  const auto moved = texturedJpeg({.seed = 52, .gain = 1.0, .offset = 0.0});
  SceneRun run({.cameraId = 106, .rebaselineAfterMs = 900000});
  CHECK(run.show(before, 1000) == CameraHealthState::Ok);
  CHECK(run.show(moved, 1500) == CameraHealthState::Moved);
  CHECK(run.show(moved, 2500) == CameraHealthState::Moved);
  CHECK(run.show(texturedJpeg({.seed = 53, .gain = 1.0, .offset = 0.0}), 3500) ==
        CameraHealthState::Moved);
}
