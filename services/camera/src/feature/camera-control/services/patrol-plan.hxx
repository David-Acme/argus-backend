#pragma once

#include <feature/camera-control/services/ptz-lease.hxx>

#include <cstddef>
#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

struct PatrolWindow
{
  int startHour{0};
  int endHour{0};
};

struct PatrolProgress
{
  bool active{false};
  std::size_t nextStop{0};
  int64_t arrivedAtMs{0};
  int64_t lapStartedMs{0};
  int64_t restUntilMs{0};
};

struct PatrolSituation
{
  bool enabled{false};
  std::size_t stopCount{0};
  int64_t dwellMs{0};
  std::optional<PatrolWindow> window;
  int localHour{0};
  bool online{false};
  bool masked{false};
  bool moduleActive{false};
  bool allowed{false};
  int64_t everyMs{0};
  PtzLeaseState lease{};
  int64_t nowMs{0};
  PatrolProgress progress{};
};

enum class PatrolStepKind : unsigned char
{
  Idle,
  Wait,
  Advance,
  Yield,
  Rest,
  End
};

struct PatrolStep
{
  PatrolStepKind kind{PatrolStepKind::Idle};
  std::size_t stop{0};
  bool returnHome{false};
};

struct PatrolStopCheck
{
  std::vector<std::string> stops;
  std::vector<std::string> saved;
};

namespace patrol_plan
{
inline constexpr int64_t kMinDwellSeconds = 5;
inline constexpr int64_t kMaxDwellSeconds = 600;
inline constexpr int64_t kDefaultDwellSeconds = 30;
inline constexpr std::size_t kMinStops = 2;
inline constexpr std::size_t kMaxStops = 16;
inline constexpr std::size_t kMaxStopIdLength = 32;
inline constexpr int kLastHour = 23;

[[nodiscard]] bool inWindow(const std::optional<PatrolWindow>& window, int hour);

[[nodiscard]] PatrolStep decide(const PatrolSituation& situation);

[[nodiscard]] std::vector<std::string> savedPresetIds(const Json::Value& presets);

[[nodiscard]] std::vector<std::string> unknownStops(const PatrolStopCheck& check);
}
