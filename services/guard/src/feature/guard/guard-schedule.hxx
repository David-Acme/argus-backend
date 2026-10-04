#pragma once

#include <config/guard-config.hxx>
#include <feature/guard/repositories/environment/environment-query.hxx>
#include <shared/vocabulary/guard-mode.hxx>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct GuardWindow
{
  uint8_t days{0x7F};
  int startMinute{0};
  int endMinute{0};
};

struct GuardSchedule
{
  bool enabled{false};
  std::vector<GuardWindow> asleep;
  std::vector<GuardWindow> open;
  std::vector<GuardWindow> staffed;
  GuardMode closedMode{GuardMode::Away};
};

struct GuardPosture
{
  GuardMode mode{GuardMode::Home};
  bool publicPresent{false};
  bool staffOnly{false};
  std::string occupancy{"manual"};
};

struct GuardPostureInput
{
  const GuardSchedule& schedule;
  GuardMode manual{GuardMode::Home};
  std::tm local{};
};

namespace guard_schedule
{

std::vector<GuardWindow> parseWindows(const std::string& spec);

bool validWindows(const std::string& spec);

GuardSchedule parse(const GuardScheduleConfig& spec);

bool inWindows(const std::vector<GuardWindow>& windows, const std::tm& local);

GuardPosture resolve(const GuardPostureInput& input);

GuardEnvironment environmentSeed(const GuardServiceConfig& config);

GuardSchedule fromEnvironment(const GuardEnvironment& environment);

struct QuietWindow
{
  bool enabled{false};
  int startHour{22};
  int endHour{7};
};

struct QuietWindowInput
{
  const GuardEnvironment& environment;
  const GuardServiceConfig& config;
};

QuietWindow quietWindow(const QuietWindowInput& input);

bool inQuietHours(const QuietWindow& window, int hour);

}
