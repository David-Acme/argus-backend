#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <monitor/health-event.hxx>
#include <monitor/camera-health-monitor.hxx>
#include <operator/frame-source.hxx>
#include <opencv2/imgcodecs.hpp>

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
    co_return frame;
  }

  CameraFrame frame;
  int calls{0};
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
                               .thresholds = {}});
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
} // namespace

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
