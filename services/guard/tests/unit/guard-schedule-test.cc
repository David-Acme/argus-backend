#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/guard/guard-schedule.hxx>

#include <ctime>

namespace
{

std::tm at(int weekday, int hour, int minute)
{
  std::tm local{};
  local.tm_wday = weekday;
  local.tm_hour = hour;
  local.tm_min = minute;
  return local;
}

GuardSchedule restaurant()
{
  return guard_schedule::parse({.enabled = true,
                                .asleep = "",
                                .open = "tue-sun 12:00-16:00, tue-sun 20:00-24:00",
                                .staffed = "tue-sun 09:00-12:00",
                                .closedMode = "away"});
}

}

TEST_CASE("windows parse days, ranges and midnight crossings")
{
  const auto windows =
      guard_schedule::parseWindows("mon-fri 09:00-18:00, 23:00-07:00, bad");
  REQUIRE(windows.size() == 2);
  CHECK(guard_schedule::inWindows(windows, at(1, 9, 0)));
  CHECK_FALSE(guard_schedule::inWindows(windows, at(6, 10, 0)));
  CHECK(guard_schedule::inWindows(windows, at(6, 23, 30)));
  CHECK(guard_schedule::inWindows(windows, at(0, 6, 59)));
  CHECK_FALSE(guard_schedule::inWindows(windows, at(0, 7, 0)));
}

TEST_CASE("a range wraps across the end of the week")
{
  const auto windows = guard_schedule::parseWindows("fri-mon 10:00-11:00");
  CHECK(guard_schedule::inWindows(windows, at(0, 10, 30)));
  CHECK(guard_schedule::inWindows(windows, at(1, 10, 30)));
  CHECK_FALSE(guard_schedule::inWindows(windows, at(3, 10, 30)));
}

TEST_CASE("a restaurant is open, staffed or closed by its hours")
{
  const GuardSchedule schedule = restaurant();
  const auto posture = [&schedule](std::tm local, GuardMode manual) {
    return guard_schedule::resolve(
        {.schedule = schedule, .manual = manual, .local = local});
  };

  const auto lunch = posture(at(3, 13, 0), GuardMode::Home);
  CHECK(lunch.publicPresent);
  CHECK(lunch.occupancy == "open");

  const auto prep = posture(at(3, 10, 0), GuardMode::Home);
  CHECK(prep.staffOnly);
  CHECK(prep.mode == GuardMode::Home);

  const auto night = posture(at(3, 2, 0), GuardMode::Home);
  CHECK(night.mode == GuardMode::Away);
  CHECK(night.occupancy == "closed");

  const auto monday = posture(at(1, 13, 0), GuardMode::Home);
  CHECK(monday.occupancy == "closed");

  const auto armed = posture(at(3, 13, 0), GuardMode::Armed);
  CHECK(armed.mode == GuardMode::Armed);
  CHECK_FALSE(armed.publicPresent);
}

TEST_CASE("a home turns to night while its residents sleep")
{
  const GuardSchedule schedule = guard_schedule::parse(
      {.enabled = true,
       .asleep = "23:00-07:00",
       .open = "",
       .staffed = "",
       .closedMode = "away"});
  const auto posture = [&schedule](std::tm local, GuardMode manual) {
    return guard_schedule::resolve(
        {.schedule = schedule, .manual = manual, .local = local});
  };
  CHECK(posture(at(2, 1, 0), GuardMode::Home).mode == GuardMode::Night);
  CHECK(posture(at(2, 15, 0), GuardMode::Home).mode == GuardMode::Home);
  CHECK(posture(at(2, 1, 0), GuardMode::Away).mode == GuardMode::Away);
}

TEST_CASE("a disabled schedule leaves the manual mode alone")
{
  const GuardSchedule schedule = guard_schedule::parse({});
  const auto posture = guard_schedule::resolve(
      {.schedule = schedule, .manual = GuardMode::Away, .local = at(3, 13, 0)});
  CHECK(posture.mode == GuardMode::Away);
  CHECK(posture.occupancy == "manual");
}
