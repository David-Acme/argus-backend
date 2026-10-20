#include <doctest/doctest.h>

#include <camera/camera-errors.hxx>
#include <feature/watch/services/microphone-hub.hxx>
#include <feature/watch/services/speaker-program.hxx>
#include <feature/watch/services/speaker-session.hxx>
#include <feature/watch/infra/guard-radar-source.hxx>
#include <feature/watch/services/watch-aim-plan.hxx>
#include <feature/watch/services/watch-aim.hxx>
#include <feature/watch/services/watch-gate.hxx>
#include <shared/services/stream/person-sightings.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <test-support/value-of.hxx>

namespace
{

namespace v1 = argus::camera::v1;

WatchGateInput openGate()
{
  return {.known = true,
          .enabled = true,
          .moduleActive = true,
          .audioAllowed = true,
          .householdWatchman = true,
          .cameraWatchman = true,
          .noAudio = false,
          .masked = false};
}

struct ScriptedReads
{
  std::mutex mutex;
  int opened{0};
  int readsBeforeEnd{3};
};

class ScriptedSource final : public MicrophoneSource
{
public:
  explicit ScriptedSource(std::shared_ptr<ScriptedReads> script) : script_(std::move(script)) {}

  bool open() override
  {
    std::scoped_lock lock(script_->mutex);
    ++script_->opened;
    left_ = script_->readsBeforeEnd;
    return true;
  }

  MicrophoneRead read(std::chrono::milliseconds) override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (left_-- <= 0)
      return {.status = MicrophoneReadStatus::Ended, .samples = {}};
    return {.status = MicrophoneReadStatus::Samples, .samples = std::vector<int16_t>(512, 7)};
  }

  void close() override {}

private:
  std::shared_ptr<ScriptedReads> script_;
  int left_{0};
};

CameraWatchConfig quickConfig()
{
  CameraWatchConfig config;
  config.micFrameMs = 32;
  config.micReconnectMs = 20;
  config.micReconnectMaxMs = 40;
  config.micLingerMs = 50;
  config.gateRecheckMs = 10;
  config.earconMs = 80;
  config.leadSilenceMs = 80;
  config.tailPadMs = 40;
  config.packetMs = 40;
  config.speakIdleMs = 2000;
  return config;
}

struct LineLog
{
  std::mutex mutex;
  std::vector<size_t> packets;
  int opened{0};
  int closed{0};
};

class RecordingLine final : public ICameraTalkLine
{
public:
  explicit RecordingLine(std::shared_ptr<LineLog> log) : log_(std::move(log)) {}

  DriverResult open() override
  {
    std::scoped_lock lock(log_->mutex);
    ++log_->opened;
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  DriverResult write(std::span<const int16_t> pcm8k) override
  {
    std::scoped_lock lock(log_->mutex);
    log_->packets.push_back(pcm8k.size());
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  void close() override
  {
    std::scoped_lock lock(log_->mutex);
    ++log_->closed;
  }

private:
  std::shared_ptr<LineLog> log_;
};

class SpeakingDriver final : public ICameraDriver
{
public:
  SpeakingDriver(std::shared_ptr<LineLog> log, bool busy) : log_(std::move(log)), busy_(busy) {}

  Json::Value capabilities() const override { return {}; }
  DriverResult status() override { return {}; }
  DriverResult presets() override { return {}; }
  DriverResult move(const DriverMoveInput&) override { return {}; }
  DriverResult preset(const DriverPresetInput&) override { return {}; }
  DriverResult settings(const DriverSettingsInput&) override { return {}; }
  DriverResult speak(const DriverSpeakInput&) override { return {}; }

  TalkLineOpen talkLine() override
  {
    if (busy_)
      return {.line = nullptr, .error = std::string(CameraErrors::TalkLineBusy.message)};
    return {.line = std::make_unique<RecordingLine>(log_), .error = {}};
  }

private:
  std::shared_ptr<LineLog> log_;
  bool busy_{false};
};

struct EventLog
{
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<v1::SpeakState> states;

  void add(v1::SpeakState state)
  {
    {
      std::scoped_lock lock(mutex);
      states.push_back(state);
    }
    changed.notify_all();
  }

  bool waitFor(v1::SpeakState state)
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5), [&] {
      return std::ranges::find(states, state) != states.end();
    });
  }
};

}

TEST_CASE("the watch gate refuses in the order the switches are owned")
{
  CHECK(watchGateVerdict(openGate()) == v1::WATCH_REFUSAL_NONE);

  WatchGateInput gate = openGate();
  gate.known = false;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_UNKNOWN_CAMERA);
  gate = openGate();
  gate.moduleActive = false;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_MODULE_OFF);
  gate = openGate();
  gate.householdWatchman = false;
  gate.audioAllowed = false;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_WATCHMAN_OFF);
  gate = openGate();
  gate.audioAllowed = false;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_AUDIO_WITHHELD);
  gate = openGate();
  gate.cameraWatchman = false;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_CAMERA_OFF);
  gate = openGate();
  gate.noAudio = true;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_NO_AUDIO);
  gate = openGate();
  gate.masked = true;
  CHECK(watchGateVerdict(gate) == v1::WATCH_REFUSAL_MASKED);
  CHECK(watchRefusalName(v1::WATCH_REFUSAL_MASKED) == "WATCH_REFUSAL_MASKED");
}

TEST_CASE("the speaker prologue pads the start drop with an earcon and silence")
{
  const std::vector<int16_t> withEarcon =
      speakerPrologue({.rate = 8000, .earconMs = 300, .leadSilenceMs = 700, .earcon = true});
  CHECK(withEarcon.size() == 8000);
  CHECK(std::ranges::any_of(withEarcon.begin(), withEarcon.begin() + 2400, [](int16_t s) { return s != 0; }));
  CHECK(std::ranges::all_of(withEarcon.begin() + 2400, withEarcon.end(), [](int16_t s) { return s == 0; }));

  const std::vector<int16_t> silent =
      speakerPrologue({.rate = 8000, .earconMs = 300, .leadSilenceMs = 700, .earcon = false});
  CHECK(silent.size() == 8000);
  CHECK(std::ranges::all_of(silent, [](int16_t s) { return s == 0; }));
  CHECK(silenceOf(8000, 300).size() == 2400);
  CHECK(earconTone(8000, 0).empty());
}

TEST_CASE("speaker pcm is read little-endian")
{
  const std::string bytes{"\x01\x00\xff\xff\x2c\x01", 6};
  CHECK(pcmSamplesOf(bytes) == std::vector<int16_t>{1, -1, 300});
  CHECK(pcmSamplesOf(std::string_view("\x01", 1)).empty());
}

TEST_CASE("one reader feeds every subscriber of a camera and reconnects with a gap")
{
  auto script = std::make_shared<ScriptedReads>();
  std::atomic<bool> open{true};
  MicrophoneHub hub({.sources = [script](int64_t) { return std::make_unique<ScriptedSource>(script); },
                     .gate = [&open](int64_t) { return open.load() ? v1::WATCH_REFUSAL_NONE : v1::WATCH_REFUSAL_MASKED; },
                     .config = quickConfig()});

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<bool> gaps;
  std::vector<size_t> sizes;
  int second = 0;
  std::optional<v1::WatchRefusal> closedWith;
  const MicrophoneSubscribed first = hub.subscribe(
      {.cameraId = 3,
       .onFrame = [&](const MicrophoneChunk& chunk) {
         std::scoped_lock lock(mutex);
         sizes.push_back(chunk.pcm.size());
         gaps.push_back(chunk.gap);
         changed.notify_all();
       },
       .onClosed = [&](v1::WatchRefusal refusal) {
         std::scoped_lock lock(mutex);
         closedWith = refusal;
         changed.notify_all();
       }});
  REQUIRE(first.id != 0);
  const MicrophoneSubscribed other = hub.subscribe({.cameraId = 3,
                                                    .onFrame = [&](const MicrophoneChunk&) {
                                                      std::scoped_lock lock(mutex);
                                                      ++second;
                                                    },
                                                    .onClosed = {}});
  REQUIRE(other.id != 0);
  CHECK(hub.readers() == 1);

  {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return gaps.size() >= 5; }));
    CHECK_FALSE(gaps.front());
    CHECK(std::ranges::all_of(sizes, [](size_t size) { return size == 512; }));
    CHECK(std::ranges::find(gaps, true) != gaps.end());
  }
  {
    std::scoped_lock lock(script->mutex);
    CHECK(script->opened >= 2);
  }

  open.store(false);
  {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return closedWith.has_value(); }));
    CHECK(*closedWith == v1::WATCH_REFUSAL_MASKED);
    CHECK(second > 0);
  }
  hub.unsubscribe(first.id);
  hub.unsubscribe(other.id);

  const MicrophoneSubscribed refused = hub.subscribe({.cameraId = 3, .onFrame = [](const MicrophoneChunk&) {}, .onClosed = {}});
  CHECK(refused.id == 0);
  CHECK(refused.refusal == v1::WATCH_REFUSAL_MASKED);
  hub.shutdown();
  CHECK(hub.readers() == 0);
}

TEST_CASE("a camera in use answers busy and a free one plays prologue, body and tail")
{
  auto busyLog = std::make_shared<LineLog>();
  EventLog busyEvents;
  {
    SpeakerSession busy({.cameraId = 1,
                         .sourceRate = 8000,
                         .earcon = true,
                         .config = quickConfig(),
                         .driver = std::make_shared<SpeakingDriver>(busyLog, true),
                         .gate = {},
                         .emit = [&busyEvents](const v1::SpeakEvent& event) { busyEvents.add(event.state()); }});
    busy.start();
    CHECK(busyEvents.waitFor(v1::SPEAK_STATE_BUSY));
  }
  CHECK(busyLog->opened == 0);

  auto log = std::make_shared<LineLog>();
  EventLog events;
  SpeakerSession session({.cameraId = 2,
                          .sourceRate = 8000,
                          .earcon = true,
                          .config = quickConfig(),
                          .driver = std::make_shared<SpeakingDriver>(log, false),
                          .gate = [] { return v1::WATCH_REFUSAL_NONE; },
                          .emit = [&events](const v1::SpeakEvent& event) { events.add(event.state()); }});
  session.start();
  REQUIRE(events.waitFor(v1::SPEAK_STATE_READY));
  const std::vector<int16_t> body(800, 100);
  session.push(body);
  session.finishInput();
  REQUIRE(events.waitFor(v1::SPEAK_STATE_DONE));
  session.join();
  std::scoped_lock lock(log->mutex);
  CHECK(log->opened == 1);
  CHECK(log->closed == 1);
  REQUIRE(log->packets.size() == 8);
  CHECK(std::ranges::all_of(log->packets, [](size_t samples) { return samples == 320; }));
}

TEST_CASE("a person already in the middle of the frame needs no move")
{
  const AimGeometry geometry{.horizontalFovDegrees = 100.0, .verticalFovDegrees = 56.0, .deadZone = 0.05};
  const PersonSighting centered{.cameraId = 1,
                                .trackId = 1,
                                .personId = 0,
                                .known = false,
                                .centerX = 0.52F,
                                .centerY = 0.6F,
                                .width = 0.1F,
                                .height = 0.6F,
                                .night = false,
                                .atMs = 1000};
  const AimStep step = aimStepFor(centered, geometry);
  CHECK(step.centered);
  CHECK(step.pan == 0);
  CHECK(step.tilt == 0);
}

TEST_CASE("a person on the right turns the camera right by the share of the field of view")
{
  const AimGeometry geometry{.horizontalFovDegrees = 100.0, .verticalFovDegrees = 56.0, .deadZone = 0.05};
  const PersonSighting right{.cameraId = 1,
                             .trackId = 1,
                             .personId = 0,
                             .known = false,
                             .centerX = 0.9F,
                             .centerY = 0.6F,
                             .width = 0.1F,
                             .height = 0.6F,
                             .night = false,
                             .atMs = 1000};
  const AimStep step = aimStepFor(right, geometry);
  CHECK_FALSE(step.centered);
  CHECK(step.pan == 40);
  CHECK(step.tilt == 0);
}

TEST_CASE("a person high in the frame tilts the camera up toward the face")
{
  const AimGeometry geometry{.horizontalFovDegrees = 100.0, .verticalFovDegrees = 56.0, .deadZone = 0.05};
  const PersonSighting high{.cameraId = 1,
                            .trackId = 1,
                            .personId = 0,
                            .known = false,
                            .centerX = 0.5F,
                            .centerY = 0.2F,
                            .width = 0.1F,
                            .height = 0.2F,
                            .night = false,
                            .atMs = 1000};
  const AimStep step = aimStepFor(high, geometry);
  CHECK_FALSE(step.centered);
  CHECK(step.pan == 0);
  CHECK(step.tilt > 0);
}

TEST_CASE("the aim takes the newest sighting and ignores those before the camera last moved")
{
  const std::vector<PersonSighting> seen{
      {.cameraId = 1, .trackId = 1, .personId = 0, .known = false, .centerX = 0.1F, .centerY = 0.5F,
       .width = 0.1F, .height = 0.5F, .night = false, .atMs = 1000},
      {.cameraId = 1, .trackId = 2, .personId = 0, .known = false, .centerX = 0.8F, .centerY = 0.5F,
       .width = 0.1F, .height = 0.5F, .night = false, .atMs = 3000},
      {.cameraId = 1, .trackId = 3, .personId = 0, .known = false, .centerX = 0.3F, .centerY = 0.5F,
       .width = 0.3F, .height = 0.6F, .night = false, .atMs = 3000}};
  const auto best = valueOf(bestSighting({.sightings = seen, .notBeforeMs = 2000}));
  CHECK(best.trackId == 3);
  CHECK_FALSE(bestSighting({.sightings = seen, .notBeforeMs = 4000}).has_value());
}

TEST_CASE("preset ids are read from both the ONVIF and the Tapo shapes")
{
  Json::Value onvif(Json::objectValue);
  onvif["preset"]["id"].append("a");
  onvif["preset"]["id"].append("b");
  CHECK(presetIdsOf(onvif) == std::vector<std::string>{"a", "b"});
  Json::Value tapo(Json::objectValue);
  tapo["preset"]["preset"]["id"].append("1");
  tapo["preset"]["preset"]["id"].append(2);
  CHECK(presetIdsOf(tapo) == std::vector<std::string>{"1", "2"});
  CHECK(presetIdsOf(Json::Value()).empty());
}

TEST_CASE("the sightings book keeps a camera's people for the hold and forgets them after")
{
  PersonSightings& book = PersonSightings::instance();
  book.forget(901);
  book.configure(45000);
  book.note({.cameraId = 901, .trackId = 1, .personId = 7, .known = true, .centerX = 0.5F, .centerY = 0.5F,
             .width = 0.1F, .height = 0.5F, .night = true, .atMs = 1000});
  book.note({.cameraId = 901, .trackId = 2, .personId = 0, .known = false, .centerX = 0.5F, .centerY = 0.5F,
             .width = 0.1F, .height = 0.5F, .night = false, .atMs = 60000});
  const std::vector<PersonSighting> recent = book.recent({.cameraId = 901, .sinceMs = 0});
  REQUIRE(recent.size() == 1);
  CHECK(recent.front().trackId == 2);
  book.forget(901);
  CHECK(book.recent({.cameraId = 901, .sinceMs = 0}).empty());
}

TEST_CASE("the aimer refuses a camera the gate closes and one without pan and tilt")
{
  auto board = std::make_shared<WatchCameraBoard>();
  board->put({.id = 5, .name = "sala", .enabled = true, .watchman = true, .noAudio = false, .talk = true,
              .panTilt = false, .tvExclusion = false});
  WatchAimer closed({.board = board,
                     .gate = [](int64_t) { return v1::WATCH_REFUSAL_MASKED; },
                     .config = {},
                     .schedule = {},
                     .radar = {}});
  CHECK(closed.find({.cameraId = 5, .conversationId = "c1"}).outcome() == v1::AIM_OUTCOME_REFUSED);
  WatchAimer open({.board = board,
                   .gate = [](int64_t) { return v1::WATCH_REFUSAL_NONE; },
                   .config = {},
                   .schedule = {},
                   .radar = {}});
  CHECK(open.find({.cameraId = 5, .conversationId = "c1"}).outcome() == v1::AIM_OUTCOME_NO_PTZ);
  CHECK(open.release({.cameraId = 5, .conversationId = "c1"}).detail() == "not_held");
}

TEST_CASE("the aimer never turns into a no-aim sector and holds instead")
{
  const std::vector<NoAimSector> sectors{
      {.name = "vecino", .panFrom = 30.0, .panTo = 90.0, .tiltFrom = -45.0, .tiltTo = 45.0}};
  CHECK(watch_aim_plan::holdFor({.position = {.pan = 0.0, .tilt = 0.0},
                                 .positionKnown = true,
                                 .delta = {.pan = 40.0, .tilt = 0.0},
                                 .sectors = sectors,
                                 .wearAllows = true,
                                 .leaseHeld = true}) == AimHold::NoAimSector);
  CHECK(watch_aim_plan::holdFor({.position = {.pan = 0.0, .tilt = 0.0},
                                 .positionKnown = true,
                                 .delta = {.pan = -40.0, .tilt = 0.0},
                                 .sectors = sectors,
                                 .wearAllows = true,
                                 .leaseHeld = true}) == AimHold::None);
  CHECK(watch_aim_plan::holdFor({.position = {.pan = 50.0, .tilt = 0.0},
                                 .positionKnown = true,
                                 .delta = {.pan = -40.0, .tilt = 0.0},
                                 .sectors = sectors,
                                 .wearAllows = true,
                                 .leaseHeld = true}) == AimHold::None);
  CHECK(aimHoldName(AimHold::NoAimSector) == "no_aim_sector");
}

TEST_CASE("an unknown position with sectors holds, and without sectors it may move")
{
  const std::vector<NoAimSector> sectors{
      {.name = "calle", .panFrom = -90.0, .panTo = -60.0, .tiltFrom = -45.0, .tiltTo = 45.0}};
  CHECK(watch_aim_plan::holdFor({.position = {},
                                 .positionKnown = false,
                                 .delta = {.pan = 10.0, .tilt = 0.0},
                                 .sectors = sectors,
                                 .wearAllows = true,
                                 .leaseHeld = true}) == AimHold::PositionUnknown);
  CHECK(watch_aim_plan::holdFor({.position = {},
                                 .positionKnown = false,
                                 .delta = {.pan = 10.0, .tilt = 0.0},
                                 .sectors = {},
                                 .wearAllows = true,
                                 .leaseHeld = true}) == AimHold::None);
}

TEST_CASE("the aimer yields to the lease first and to the wear caps next")
{
  CHECK(watch_aim_plan::holdFor({.position = {},
                                 .positionKnown = true,
                                 .delta = {.pan = 10.0, .tilt = 0.0},
                                 .sectors = {},
                                 .wearAllows = false,
                                 .leaseHeld = false}) == AimHold::Lease);
  CHECK(watch_aim_plan::holdFor({.position = {},
                                 .positionKnown = true,
                                 .delta = {.pan = 10.0, .tilt = 0.0},
                                 .sectors = {},
                                 .wearAllows = false,
                                 .leaseHeld = true}) == AimHold::Wear);
}

TEST_CASE("an LD2450 target becomes a bearing from the sensor's axis")
{
  CHECK(valueOf(watch_aim_plan::radarBearingDeg({.xMm = 0.0, .yMm = 2000.0}, 6000.0)) == doctest::Approx(0.0));
  CHECK(valueOf(watch_aim_plan::radarBearingDeg({.xMm = 1500.0, .yMm = 1500.0}, 6000.0)) == doctest::Approx(45.0));
  CHECK(valueOf(watch_aim_plan::radarBearingDeg({.xMm = -1000.0, .yMm = 1732.05}, 6000.0)) ==
        doctest::Approx(-30.0).epsilon(0.001));
  CHECK_FALSE(watch_aim_plan::radarBearingDeg({.xMm = 0.0, .yMm = 0.0}, 6000.0).has_value());
  CHECK_FALSE(watch_aim_plan::radarBearingDeg({.xMm = 300.0, .yMm = -500.0}, 6000.0).has_value());
  CHECK_FALSE(watch_aim_plan::radarBearingDeg({.xMm = 0.0, .yMm = 7000.0}, 6000.0).has_value());
}

TEST_CASE("the radar aim adds the sensor's pan offset and turns by the difference from the camera's pan")
{
  const std::vector<RadarFix> fixes{
      {.sensorId = 3, .panOffsetDeg = 20.0, .targets = {{.xMm = 1500.0, .yMm = 1500.0}}, .atMs = 9000}};
  const RadarAim placed = watch_aim_plan::radarAim({.fixes = fixes,
                                                    .position = {.pan = 10.0, .tilt = 5.0},
                                                    .positionKnown = true,
                                                    .nowMs = 10000,
                                                    .maxAgeMs = 3000,
                                                    .maxRangeMm = 6000.0,
                                                    .deadZoneDeg = 5.0});
  CHECK(placed.placed);
  CHECK(placed.move);
  CHECK(placed.sensorId == 3);
  CHECK(placed.targetPan == doctest::Approx(65.0));
  CHECK(placed.delta.pan == doctest::Approx(55.0));
  CHECK(placed.delta.tilt == doctest::Approx(0.0));
}

TEST_CASE("the radar aim picks the target nearest the camera's pan and ignores stale and empty slots")
{
  const std::vector<RadarFix> fixes{
      {.sensorId = 1,
       .panOffsetDeg = 0.0,
       .targets = {{.xMm = 0.0, .yMm = 0.0}, {.xMm = 2000.0, .yMm = 2000.0}, {.xMm = -200.0, .yMm = 3000.0}},
       .atMs = 9500},
      {.sensorId = 2, .panOffsetDeg = 0.0, .targets = {{.xMm = 10.0, .yMm = 3000.0}}, .atMs = 1000}};
  const RadarAim placed = watch_aim_plan::radarAim({.fixes = fixes,
                                                    .position = {.pan = 0.0, .tilt = 0.0},
                                                    .positionKnown = true,
                                                    .nowMs = 10000,
                                                    .maxAgeMs = 3000,
                                                    .maxRangeMm = 6000.0,
                                                    .deadZoneDeg = 5.0});
  CHECK(placed.placed);
  CHECK(placed.sensorId == 1);
  CHECK_FALSE(placed.move);
  CHECK(placed.reason == "facing");
  const std::vector<RadarFix> stale{
      {.sensorId = 2, .panOffsetDeg = 0.0, .targets = {{.xMm = 10.0, .yMm = 3000.0}}, .atMs = 1000}};
  const RadarAim none = watch_aim_plan::radarAim({.fixes = stale,
                                                  .position = {},
                                                  .positionKnown = true,
                                                  .nowMs = 10000,
                                                  .maxAgeMs = 3000,
                                                  .maxRangeMm = 6000.0,
                                                  .deadZoneDeg = 5.0});
  CHECK_FALSE(none.placed);
  CHECK(none.reason == "no_target");
}

TEST_CASE("the radar places a person but an unknown position cannot be turned from")
{
  const std::vector<RadarFix> fixes{
      {.sensorId = 1, .panOffsetDeg = 0.0, .targets = {{.xMm = 1000.0, .yMm = 1000.0}}, .atMs = 10000}};
  const RadarAim placed = watch_aim_plan::radarAim({.fixes = fixes,
                                                    .position = {},
                                                    .positionKnown = false,
                                                    .nowMs = 10000,
                                                    .maxAgeMs = 3000,
                                                    .maxRangeMm = 6000.0,
                                                    .deadZoneDeg = 5.0});
  CHECK(placed.placed);
  CHECK_FALSE(placed.move);
  CHECK(placed.reason == "position_unknown");
}

TEST_CASE("guard's sensor targets read as radar fixes")
{
  argus::guard::v1::SensorTargets sensor;
  sensor.set_sensor_id(8);
  sensor.set_pan_offset_deg(-12.5);
  sensor.set_at_ms(4000);
  auto* target = sensor.add_targets();
  target->set_x_mm(-400.0);
  target->set_y_mm(2500.0);
  const std::vector<RadarFix> fixes = radarFixesOf({sensor});
  REQUIRE(fixes.size() == 1);
  CHECK(fixes.front().sensorId == 8);
  CHECK(fixes.front().panOffsetDeg == doctest::Approx(-12.5));
  CHECK(fixes.front().atMs == 4000);
  REQUIRE(fixes.front().targets.size() == 1);
  CHECK(fixes.front().targets.front().xMm == doctest::Approx(-400.0));
  CHECK(fixes.front().targets.front().yMm == doctest::Approx(2500.0));
}
