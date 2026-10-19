#include <doctest/doctest.h>

#include <errors/validation-exception.hxx>
#include <feature/camera-control/dtos/camera-patrol-dto.hxx>
#include <feature/camera-control/services/patrol-plan.hxx>
#include <text/json-util.hxx>

#include <string>
#include <vector>

namespace
{
PatrolSituation patrolling(PatrolProgress progress)
{
  return {.enabled = true,
          .stopCount = 3,
          .dwellMs = 30000,
          .window = std::nullopt,
          .localHour = 12,
          .online = true,
          .masked = false,
          .moduleActive = true,
          .lease = {},
          .nowMs = 100000,
          .progress = progress};
}

PatrolProgress at(std::size_t nextStop, int64_t arrivedAtMs)
{
  return {.active = true, .nextStop = nextStop, .arrivedAtMs = arrivedAtMs};
}

bool refused(const std::string& body, const std::string& field)
{
  try {
    static_cast<void>(CameraPatrolDto::fromJson(json_util::fromString(body)));
  }
  catch (const ValidationException& error) {
    return error.errors().contains(field);
  }
  return false;
}
}

TEST_CASE("a patrol starts at its first stop and walks the presets in order")
{
  const PatrolStep first = patrol_plan::decide(patrolling({}));
  CHECK(first.kind == PatrolStepKind::Advance);
  CHECK(first.stop == 0);

  auto situation = patrolling(at(1, 90000));
  situation.lease.holder = PtzHolder::Argus;
  CHECK(patrol_plan::decide(situation).kind == PatrolStepKind::Wait);

  situation.nowMs = 119999;
  CHECK(patrol_plan::decide(situation).kind == PatrolStepKind::Wait);

  situation.nowMs = 120000;
  const PatrolStep next = patrol_plan::decide(situation);
  CHECK(next.kind == PatrolStepKind::Advance);
  CHECK(next.stop == 1);
}

TEST_CASE("the patrol wraps around after its last stop, and a shorter list wraps too")
{
  auto situation = patrolling(at(3, 0));
  situation.lease.holder = PtzHolder::Argus;
  CHECK(patrol_plan::decide(situation).stop == 0);

  situation.progress.nextStop = 7;
  CHECK(patrol_plan::decide(situation).stop == 1);
}

TEST_CASE("a viewer or a person on the lease makes the patrol yield instead of fighting")
{
  for (const PtzHolder holder : {PtzHolder::Viewer, PtzHolder::Person}) {
    auto idle = patrolling({});
    idle.lease.holder = holder;
    CHECK(patrol_plan::decide(idle).kind == PatrolStepKind::Yield);

    auto running = patrolling(at(2, 0));
    running.lease.holder = holder;
    const PatrolStep step = patrol_plan::decide(running);
    CHECK(step.kind == PatrolStepKind::Yield);
    CHECK(step.stop == 2);
  }
}

TEST_CASE("the patrol waits out the park time a person leaves, and another Argus move")
{
  auto parked = patrolling(at(1, 0));
  parked.lease = {.holder = PtzHolder::None, .userId = 0, .sinceMs = 0, .parkUntilMs = 100001};
  CHECK(patrol_plan::decide(parked).kind == PatrolStepKind::Yield);

  parked.nowMs = 100001;
  CHECK(patrol_plan::decide(parked).kind == PatrolStepKind::Advance);

  auto homing = patrolling({});
  homing.lease.holder = PtzHolder::Argus;
  CHECK(patrol_plan::decide(homing).kind == PatrolStepKind::Yield);
}

TEST_CASE("privacy, a lost camera and a switched-off module stop the patrol without a way home")
{
  auto masked = patrolling(at(1, 0));
  masked.masked = true;
  const PatrolStep privacy = patrol_plan::decide(masked);
  CHECK(privacy.kind == PatrolStepKind::End);
  CHECK_FALSE(privacy.returnHome);

  auto offline = patrolling(at(1, 0));
  offline.online = false;
  CHECK(patrol_plan::decide(offline).kind == PatrolStepKind::End);
  CHECK_FALSE(patrol_plan::decide(offline).returnHome);

  auto module = patrolling(at(1, 0));
  module.moduleActive = false;
  CHECK(patrol_plan::decide(module).kind == PatrolStepKind::End);
  CHECK_FALSE(patrol_plan::decide(module).returnHome);

  auto idle = patrolling({});
  idle.masked = true;
  CHECK(patrol_plan::decide(idle).kind == PatrolStepKind::Idle);
}

TEST_CASE("a closed window, a disabled patrol and an empty list end the patrol on the way home")
{
  auto closing = patrolling(at(1, 0));
  closing.window = PatrolWindow{.startHour = 22, .endHour = 6};
  closing.localHour = 6;
  const PatrolStep end = patrol_plan::decide(closing);
  CHECK(end.kind == PatrolStepKind::End);
  CHECK(end.returnHome);

  auto disabled = patrolling(at(1, 0));
  disabled.enabled = false;
  CHECK(patrol_plan::decide(disabled).returnHome);

  auto empty = patrolling(at(1, 0));
  empty.stopCount = 0;
  CHECK(patrol_plan::decide(empty).kind == PatrolStepKind::End);

  auto never = patrolling({});
  never.stopCount = 0;
  CHECK(patrol_plan::decide(never).kind == PatrolStepKind::Idle);
}

TEST_CASE("the daily window holds its start hour, drops its end hour and may cross midnight")
{
  const PatrolWindow day{.startHour = 8, .endHour = 18};
  CHECK_FALSE(patrol_plan::inWindow(day, 7));
  CHECK(patrol_plan::inWindow(day, 8));
  CHECK(patrol_plan::inWindow(day, 17));
  CHECK_FALSE(patrol_plan::inWindow(day, 18));

  const PatrolWindow night{.startHour = 22, .endHour = 6};
  CHECK(patrol_plan::inWindow(night, 22));
  CHECK(patrol_plan::inWindow(night, 23));
  CHECK(patrol_plan::inWindow(night, 0));
  CHECK(patrol_plan::inWindow(night, 5));
  CHECK_FALSE(patrol_plan::inWindow(night, 6));
  CHECK_FALSE(patrol_plan::inWindow(night, 21));

  CHECK(patrol_plan::inWindow(std::nullopt, 3));

  auto outside = patrolling({});
  outside.window = day;
  outside.localHour = 20;
  CHECK(patrol_plan::decide(outside).kind == PatrolStepKind::Idle);
}

TEST_CASE("the saved presets are read in both shapes the drivers answer, and unknown stops are named once")
{
  const Json::Value onvif = json_util::fromString(R"({"preset":{"id":["a","b"],"name":["Door","Yard"]}})");
  CHECK(patrol_plan::savedPresetIds(onvif) == std::vector<std::string>{"a", "b"});

  const Json::Value tapo = json_util::fromString(R"({"preset":{"preset":{"id":["1",2],"name":["x","y"]}}})");
  CHECK(patrol_plan::savedPresetIds(tapo) == std::vector<std::string>{"1", "2"});

  CHECK(patrol_plan::savedPresetIds(Json::Value()).empty());

  const auto unknown = patrol_plan::unknownStops({.stops = {"1", "9", "2", "9"}, .saved = {"1", "2"}});
  CHECK(unknown == std::vector<std::string>{"9"});
}

TEST_CASE("a patrol body is validated before anything reaches the camera")
{
  const auto dto = CameraPatrolDto::fromJson(json_util::fromString(
      R"({"stops":["1","2","3"],"dwellSeconds":45,"enabled":true,"window":{"startHour":22,"endHour":6}})"));
  REQUIRE(dto.stops.has_value());
  CHECK(dto.stops->size() == 3);
  CHECK(dto.dwellSeconds == 45);
  CHECK(dto.enabled == true);
  CHECK(dto.windowGiven);
  REQUIRE(dto.window.has_value());
  CHECK(dto.window->startHour == 22);

  const auto cleared = CameraPatrolDto::fromJson(json_util::fromString(R"({"window":null})"));
  CHECK(cleared.windowGiven);
  CHECK_FALSE(cleared.window.has_value());
  CHECK_FALSE(cleared.stops.has_value());

  const auto untouched = CameraPatrolDto::fromJson(json_util::fromString(R"({"dwellSeconds":5})"));
  CHECK_FALSE(untouched.windowGiven);

  CHECK(refused(R"({"stops":["1"]})", "stops"));
  CHECK(refused(R"({"stops":["1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17"]})",
                "stops"));
  CHECK(refused(R"({"stops":["1",""]})", "stops"));
  CHECK(refused(R"({"stops":["1",2]})", "stops"));
  CHECK(refused(R"({"stops":"1,2"})", "stops"));
  CHECK(refused(R"({"stops":["1","1","2"]})", "stops"));
  CHECK(refused(R"({"stops":["1","2","1"]})", "stops"));
  CHECK(refused(R"({"dwellSeconds":4})", "dwellSeconds"));
  CHECK(refused(R"({"dwellSeconds":601})", "dwellSeconds"));
  CHECK(refused(R"({"dwellSeconds":"30"})", "dwellSeconds"));
  CHECK(refused(R"({"enabled":"yes"})", "enabled"));
  CHECK(refused(R"({"window":{"startHour":24,"endHour":6}})", "window"));
  CHECK(refused(R"({"window":{"startHour":6,"endHour":6}})", "window"));
  CHECK(refused(R"({"window":{"startHour":6}})", "window"));
  CHECK(refused(R"({"window":"night"})", "window"));
  CHECK_FALSE(refused(R"({"stops":["1","2"],"dwellSeconds":600})", "stops"));
}
