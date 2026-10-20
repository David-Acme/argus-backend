#include <doctest/doctest.h>

#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/aim-math.hxx>
#include <feature/camera-control/services/aim-settings.hxx>
#include <feature/camera-control/services/ptz-lease.hxx>
#include <feature/look/services/follow-service.hxx>
#include <feature/look/services/look-service.hxx>
#include <feature/look/services/look-refresh.hxx>
#include <feature/look/services/second-look.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>

#include <vector>
#include <test-support/value-of.hxx>

TEST_CASE("a box off center becomes degrees, and the dead zone keeps a centered box still")
{
  const FieldOfView fov{.horizontalDeg = 80.0, .verticalDeg = 40.0};
  const auto right = valueOf(aim_math::centerOffset({.box = {.x = 0.8, .y = 0.45, .w = 0.1, .h = 0.1}, .fov = fov, .deadZone = 0.05}));
  CHECK(right.pan == doctest::Approx(0.35 * 80.0));
  CHECK(right.tilt == doctest::Approx(0.0));
  const auto low = valueOf(aim_math::centerOffset({.box = {.x = 0.45, .y = 0.8, .w = 0.1, .h = 0.1}, .fov = fov, .deadZone = 0.05}));
  CHECK(low.tilt == doctest::Approx(-0.35 * 40.0));
  CHECK_FALSE(aim_math::centerOffset({.box = {.x = 0.47, .y = 0.47, .w = 0.06, .h = 0.06}, .fov = fov, .deadZone = 0.05})
                  .has_value());
}

TEST_CASE("a moving target is led only when its velocity agrees")
{
  const std::vector<TrackSample> steady{{.cx = 0.4, .cy = 0.5, .atMs = 0},
                                        {.cx = 0.45, .cy = 0.5, .atMs = 500},
                                        {.cx = 0.5, .cy = 0.5, .atMs = 1000},
                                        {.cx = 0.55, .cy = 0.5, .atMs = 1500}};
  const auto ahead = valueOf(aim_math::lead({.samples = steady, .minSamples = 3, .consistency = 0.7, .aheadMs = 1000}));
  CHECK(ahead.x > 0.55);
  const std::vector<TrackSample> jitter{{.cx = 0.4, .cy = 0.5, .atMs = 0},
                                        {.cx = 0.45, .cy = 0.55, .atMs = 500},
                                        {.cx = 0.42, .cy = 0.5, .atMs = 1000},
                                        {.cx = 0.46, .cy = 0.45, .atMs = 1500}};
  CHECK_FALSE(aim_math::lead({.samples = jitter, .minSamples = 3, .consistency = 0.8, .aheadMs = 1000}).has_value());
  CHECK_FALSE(aim_math::lead({.samples = std::vector<TrackSample>(steady.begin(), steady.begin() + 2),
                              .minSamples = 3,
                              .consistency = 0.7,
                              .aheadMs = 1000})
                  .has_value());
}

TEST_CASE("the wear caps hold six moves a minute and the daily cap")
{
  const std::vector<int64_t> fiveMoves{1000, 2000, 3000, 4000, 5000};
  CHECK(aim_math::wearAllows({.recentMovesMs = fiveMoves, .nowMs = 6000, .perMinute = 6, .movesToday = 5, .dailyCap = 400}));
  const std::vector<int64_t> sixMoves{1000, 2000, 3000, 4000, 5000, 5500};
  CHECK_FALSE(aim_math::wearAllows({.recentMovesMs = sixMoves, .nowMs = 6000, .perMinute = 6, .movesToday = 6, .dailyCap = 400}));
  CHECK(aim_math::wearAllows({.recentMovesMs = sixMoves, .nowMs = 70000, .perMinute = 6, .movesToday = 6, .dailyCap = 400}));
  CHECK_FALSE(aim_math::wearAllows({.recentMovesMs = {}, .nowMs = 70000, .perMinute = 6, .movesToday = 400, .dailyCap = 400}));

  AimBoard& board = AimBoard::instance();
  board.forget(901);
  for (int move = 0; move < 6; ++move) {
    REQUIRE(board.wearAllows({.cameraId = 901, .nowMs = 10000 + move, .perMinute = 6, .dailyCap = 400}));
    board.recordMove({.cameraId = 901, .delta = {.pan = 5.0, .tilt = 0.0}, .atMs = 10000 + move});
  }
  CHECK_FALSE(board.wearAllows({.cameraId = 901, .nowMs = 10010, .perMinute = 6, .dailyCap = 400}));
  CHECK(board.snapshot(901).position.pan == doctest::Approx(30.0));
  CHECK_FALSE(board.snapshot(901).atHome);
  board.arriveHome(901);
  CHECK(board.snapshot(901).atHome);
  CHECK(board.snapshot(901).position.pan == doctest::Approx(0.0));
  board.lose(901);
  CHECK_FALSE(board.snapshot(901).known);
  board.forget(901);
}

TEST_CASE("a look never centers into a no-aim sector, nor moves with an unknown position")
{
  const std::vector<NoAimSector> sectors{{.name = "Vecino", .panFrom = 20.0, .panTo = 60.0, .tiltFrom = -45.0, .tiltTo = 45.0}};
  CameraPtzConfig ptz;
  ptz.fovHorizontalDeg = 80.0;
  ptz.fovVerticalDeg = 40.0;
  ptz.deadZone = 0.05;
  const NormalizedBox right{.x = 0.85, .y = 0.45, .w = 0.1, .h = 0.1};
  const LookPlan blocked = look_plan::plan(
      {.box = right, .position = {}, .positionKnown = true, .lookable = true, .sectors = sectors, .ptz = ptz});
  CHECK_FALSE(blocked.move);
  CHECK(blocked.refusal == "no_aim_sector");
  const NormalizedBox left{.x = 0.05, .y = 0.45, .w = 0.1, .h = 0.1};
  const LookPlan allowed = look_plan::plan(
      {.box = left, .position = {}, .positionKnown = true, .lookable = true, .sectors = sectors, .ptz = ptz});
  CHECK(allowed.move);
  CHECK(allowed.delta.pan < 0.0);
  CHECK(look_plan::plan({.box = left, .position = {}, .positionKnown = false, .lookable = true, .sectors = sectors, .ptz = ptz})
            .refusal == "position_unknown");
  CHECK(look_plan::plan({.box = left, .position = {}, .positionKnown = true, .lookable = false, .sectors = sectors, .ptz = ptz})
            .refusal == "not_lookable");
  const NormalizedBox centered = look_plan::centered(right);
  CHECK(centered.x == doctest::Approx(0.45));
  CHECK(centered.w == doctest::Approx(0.1));
}

TEST_CASE("frames away from home black out the part of a no-aim sector in view")
{
  const std::vector<NoAimSector> sectors{{.name = "Ventana", .panFrom = 10.0, .panTo = 30.0, .tiltFrom = -20.0, .tiltTo = 20.0}};
  const auto columns = aim_math::blackout({.position = {.pan = 20.0, .tilt = 0.0},
                                           .fov = {.horizontalDeg = 80.0, .verticalDeg = 40.0},
                                           .sectors = sectors,
                                           .width = 800});
  REQUIRE(columns.size() == 1);
  CHECK(columns.front().from == 300);
  CHECK(columns.front().to == 500);
  CHECK(aim_math::blackout({.position = {.pan = -120.0, .tilt = 0.0},
                            .fov = {.horizontalDeg = 80.0, .verticalDeg = 40.0},
                            .sectors = sectors,
                            .width = 800})
            .empty());
}

TEST_CASE("the lease keeps a person above a viewer above Argus, and Argus waits out the park time")
{
  PtzLease& lease = PtzLease::instance();
  lease.configure({.parkMs = 60000, .idleReleaseMs = 60000});
  lease.forget(902);
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Viewer, .userId = 7, .atMs = 1000}) == PtzClaimResult::Granted);
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Argus, .userId = 0, .atMs = 1500}) == PtzClaimResult::Refused);
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Person, .userId = 8, .atMs = 2000}) == PtzClaimResult::Preempted);
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Viewer, .userId = 7, .atMs = 2500}) == PtzClaimResult::Refused);
  lease.release({.cameraId = 902, .holder = PtzHolder::Person, .userId = 8, .atMs = 3000});
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Argus, .userId = 0, .atMs = 30000}) == PtzClaimResult::Refused);
  CHECK(lease.claim({.cameraId = 902, .holder = PtzHolder::Argus, .userId = 0, .atMs = 64000}) == PtzClaimResult::Granted);
  CHECK(ptzHolderToString(lease.stateOf({.cameraId = 902, .atMs = 64000}).holder) == "argus");
  lease.forget(902);
}

TEST_CASE("a doubtful detection is weak, small, or a stranger while the household is protected")
{
  CHECK(second_look::doubtful({.confidence = 0.3F, .confirmConfidence = 0.45F, .boxShare = 0.1, .smallBoxShare = 0.03,
                               .unknownPerson = false, .protect = false}));
  CHECK(second_look::doubtful({.confidence = 0.8F, .confirmConfidence = 0.45F, .boxShare = 0.01, .smallBoxShare = 0.03,
                               .unknownPerson = false, .protect = false}));
  CHECK(second_look::doubtful({.confidence = 0.8F, .confirmConfidence = 0.45F, .boxShare = 0.1, .smallBoxShare = 0.03,
                               .unknownPerson = true, .protect = true}));
  CHECK_FALSE(second_look::doubtful({.confidence = 0.8F, .confirmConfidence = 0.45F, .boxShare = 0.1,
                                     .smallBoxShare = 0.03, .unknownPerson = true, .protect = false}));
  LookCooldown& cooldown = LookCooldown::instance();
  CHECK_FALSE(cooldown.cooling({.cameraId = 903, .cell = 5, .atMs = 1000}));
  cooldown.cool({.cameraId = 903, .cell = 5, .atMs = 1000}, 60000);
  CHECK(cooldown.cooling({.cameraId = 903, .cell = 5, .atMs = 30000}));
  CHECK_FALSE(cooldown.cooling({.cameraId = 903, .cell = 6, .atMs = 30000}));
  CHECK_FALSE(cooldown.cooling({.cameraId = 903, .cell = 5, .atMs = 61001}));
  CHECK(aim_math::areaCell({.x = 0.0, .y = 0.0, .w = 0.1, .h = 0.1}) == 0);
  CHECK(aim_math::areaCell({.x = 0.9, .y = 0.9, .w = 0.1, .h = 0.1}) == 15);
}

TEST_CASE("a follow keeps one target, re-acquires it at the center after its own move, and ends when lost")
{
  FollowBoard& board = FollowBoard::instance();
  board.stop(904);
  board.start({.cameraId = 904, .trackId = 3, .atMs = 1000});
  CHECK(board.following(904));
  const TrackSighting other{.cameraId = 904, .trackId = 9, .box = {.x = 0.9, .y = 0.4, .w = 0.05, .h = 0.2}, .atMs = 1100};
  CHECK_FALSE(board.sighted({.sighting = other, .fov = {}, .deadZone = 0.05, .leadMinSamples = 3, .leadConsistency = 0.7})
                  .has_value());
  const TrackSighting near{.cameraId = 904, .trackId = 3, .box = {.x = 0.47, .y = 0.4, .w = 0.06, .h = 0.2}, .atMs = 1200};
  CHECK_FALSE(board.sighted({.sighting = near, .fov = {}, .deadZone = 0.05, .leadMinSamples = 3, .leadConsistency = 0.7})
                  .has_value());
  const TrackSighting edge{.cameraId = 904, .trackId = 3, .box = {.x = 0.85, .y = 0.4, .w = 0.06, .h = 0.2}, .atMs = 1300};
  const auto step = valueOf(board.sighted({.sighting = edge, .fov = {}, .deadZone = 0.05, .leadMinSamples = 3, .leadConsistency = 0.7}));
  CHECK(step.delta.pan > 0.0);
  CHECK_FALSE(board.sighted({.sighting = edge, .fov = {}, .deadZone = 0.05, .leadMinSamples = 3, .leadConsistency = 0.7})
                  .has_value());
  board.settled(904);
  const TrackSighting again{.cameraId = 904, .trackId = 12, .box = {.x = 0.47, .y = 0.4, .w = 0.06, .h = 0.2}, .atMs = 5000};
  CHECK_FALSE(board.sighted({.sighting = again, .fov = {}, .deadZone = 0.05, .leadMinSamples = 3, .leadConsistency = 0.7})
                  .has_value());
  CHECK(board.lost({.nowMs = 9000, .lostAfterMs = 10000}).empty());
  const auto ended = board.lost({.nowMs = 15001, .lostAfterMs = 10000});
  REQUIRE(ended.size() == 1);
  CHECK(ended.front() == 904);
  CHECK_FALSE(board.following(904));
}

TEST_CASE("the aim settings live in the camera config, and a look on a PTZ camera needs a home")
{
  const std::string capabilities = R"(["ptz","presets"])";
  const std::string config = R"({"homePreset":"2","noAimSectors":[{"name":"Vecino","panFrom":10,"panTo":40,"tiltFrom":-10,"tiltTo":10}]})";
  const AimSettings settings = aim_settings::parse({.capabilities = capabilities, .config = config});
  CHECK(settings.ptz);
  CHECK(settings.lookable());
  CHECK(settings.followIntruders);
  REQUIRE(settings.noAimSectors.size() == 1);
  const std::string empty = "{}";
  CHECK_FALSE(aim_settings::parse({.capabilities = capabilities, .config = empty}).lookable());
  const AimSettingsPatch patch{.activeLook = false, .followIntruders = false, .noAimSectors = std::nullopt};
  const std::string merged = aim_settings::merged({.config = config, .patch = patch});
  const AimSettings after = aim_settings::parse({.capabilities = capabilities, .config = merged});
  CHECK_FALSE(after.activeLook);
  CHECK_FALSE(after.followIntruders);
  CHECK(after.homePreset == "2");
  CHECK(after.noAimSectors.size() == 1);
}

TEST_CASE("Argus away from home holds the health reference instead of relearning it")
{
  CameraSceneLog& log = CameraSceneLog::instance();
  log.forget(905);
  log.setArgusAway({.cameraId = 905, .away = true});
  log.beginMove({.cameraId = 905, .startMs = 1000, .expectedEndMs = 5000, .returnsHome = true});
  log.endMove({.cameraId = 905, .atMs = 5000});
  CHECK(log.sceneOf(905).argusAway);
  CHECK(log.sceneOf(905).aimedAtMs == 0);
  log.noteAimed({.cameraId = 905, .atMs = 6000});
  CHECK_FALSE(log.sceneOf(905).argusAway);
  log.forget(905);
}
