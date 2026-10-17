#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/operator/services/detection-change-gate.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
class RecordingSink final : public CameraChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAudit(const ModuleAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishResource(const CameraResourceChange& input) const override
  {
    notices.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput&) const override
  {
    co_return;
  }

  mutable std::vector<CameraResourceChange> notices;
};
}

TEST_CASE("a burst of detections announces once, and once more when the burst ends")
{
  RecordingSink sink;
  camera_change::setSink(&sink);
  int64_t at = 1'700'000'000;
  DetectionChangeGate gate({.publishSeconds = 15, .clock = [&at]() { return at; }});

  for (int step = 0; step < 5; ++step) {
    drogon::sync_wait(gate.detected(7));
    at += 1;
  }
  REQUIRE(sink.notices.size() == 1);
  CHECK(sink.notices.front().scope == ResourceScope::Overview);
  REQUIRE(sink.notices.front().ids.size() == 1);
  CHECK(sink.notices.front().ids.front() == 7);
  CHECK(sink.notices.front().user == 0);

  drogon::sync_wait(gate.flush());
  CHECK(sink.notices.size() == 1);

  at += 15;
  drogon::sync_wait(gate.flush());
  REQUIRE(sink.notices.size() == 2);
  CHECK(sink.notices.back().ids.front() == 7);

  drogon::sync_wait(gate.flush());
  CHECK(sink.notices.size() == 2);
}

TEST_CASE("two cameras in one window travel in one notice")
{
  RecordingSink sink;
  camera_change::setSink(&sink);
  int64_t at = 1'700'000'000;
  DetectionChangeGate gate({.publishSeconds = 15, .clock = [&at]() { return at; }});

  drogon::sync_wait(gate.detected(3));
  drogon::sync_wait(gate.detected(4));
  at += 1;
  drogon::sync_wait(gate.detected(3));
  drogon::sync_wait(gate.detected(4));
  REQUIRE(sink.notices.size() == 2);

  at += 15;
  drogon::sync_wait(gate.flush());
  REQUIRE(sink.notices.size() == 3);
  CHECK(sink.notices.back().ids.size() == 2);
}

TEST_CASE("a detection without a sink is quiet, and a single one leaves nothing trailing")
{
  camera_change::setSink(nullptr);
  int64_t at = 1'700'000'000;
  DetectionChangeGate gate({.publishSeconds = 15, .clock = [&at]() { return at; }});
  CHECK_NOTHROW(drogon::sync_wait(gate.detected(9)));
  CHECK_NOTHROW(drogon::sync_wait(gate.flush()));

  RecordingSink sink;
  camera_change::setSink(&sink);
  drogon::sync_wait(gate.detected(9));
  CHECK(sink.notices.size() == 1);
  at += 600;
  drogon::sync_wait(gate.flush());
  CHECK(sink.notices.size() == 1);
}
