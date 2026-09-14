#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <monitor/health-event.hxx>

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
  CHECK(health_monitor::classify(metrics, thresholds()) == CameraHealthState::Dark);
}

TEST_CASE("status names are stable")
{
  CHECK(health_monitor::statusName(CameraHealthState::Ok) == "ok");
  CHECK(health_monitor::statusName(CameraHealthState::Moved) == "moved");
  CHECK(health_monitor::statusName(CameraHealthState::Unreachable) == "unreachable");
}
