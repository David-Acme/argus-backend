#include "guard-schedule.hxx"

#include <array>
#include <optional>
#include <string_view>

namespace
{

constexpr std::array<std::string_view, 7> kDays = {"sun", "mon", "tue", "wed",
                                                    "thu", "fri", "sat"};

std::string_view trim(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

std::optional<int> dayIndex(std::string_view name)
{
  for (size_t i = 0; i < kDays.size(); ++i) {
    if (kDays[i] == name)
      return static_cast<int>(i);
  }
  return std::nullopt;
}

std::optional<uint8_t> parseDays(std::string_view text)
{
  const auto dash = text.find('-');
  if (dash == std::string_view::npos) {
    const auto day = dayIndex(text);
    if (!day)
      return std::nullopt;
    return static_cast<uint8_t>(1U << *day);
  }
  const auto first = dayIndex(text.substr(0, dash));
  const auto last = dayIndex(text.substr(dash + 1));
  if (!first || !last)
    return std::nullopt;
  uint8_t mask = 0;
  for (int day = *first;; day = (day + 1) % 7) {
    mask = static_cast<uint8_t>(mask | (1U << day));
    if (day == *last)
      break;
  }
  return mask;
}

std::optional<int> parseClock(std::string_view text)
{
  if (text.size() != 5 || text[2] != ':')
    return std::nullopt;
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (!digit(text[0]) || !digit(text[1]) || !digit(text[3]) || !digit(text[4]))
    return std::nullopt;
  const int hours = (text[0] - '0') * 10 + (text[1] - '0');
  const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
  if (hours > 24 || minutes > 59 || (hours == 24 && minutes != 0))
    return std::nullopt;
  return hours * 60 + minutes;
}

std::optional<GuardWindow> parseWindow(std::string_view text)
{
  text = trim(text);
  GuardWindow window;
  const auto space = text.find(' ');
  if (space != std::string_view::npos) {
    const auto days = parseDays(trim(text.substr(0, space)));
    if (!days)
      return std::nullopt;
    window.days = *days;
    text = trim(text.substr(space + 1));
  }
  const auto dash = text.find('-');
  if (dash == std::string_view::npos)
    return std::nullopt;
  const auto start = parseClock(trim(text.substr(0, dash)));
  const auto end = parseClock(trim(text.substr(dash + 1)));
  if (!start || !end || *start == *end)
    return std::nullopt;
  window.startMinute = *start;
  window.endMinute = *end;
  return window;
}

bool dayIn(uint8_t mask, int day)
{
  return (mask & (1U << ((day % 7 + 7) % 7))) != 0;
}

}

std::vector<GuardWindow> guard_schedule::parseWindows(const std::string& spec)
{
  std::vector<GuardWindow> windows;
  std::string_view rest(spec);
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const std::string_view part = rest.substr(0, comma);
    if (const auto window = parseWindow(part))
      windows.push_back(*window);
    if (comma == std::string_view::npos)
      break;
    rest.remove_prefix(comma + 1);
  }
  return windows;
}

bool guard_schedule::validWindows(const std::string& spec)
{
  std::string_view rest(spec);
  if (trim(rest).empty())
    return true;
  while (true) {
    const auto comma = rest.find(',');
    if (!parseWindow(rest.substr(0, comma)))
      return false;
    if (comma == std::string_view::npos)
      return true;
    rest.remove_prefix(comma + 1);
  }
}

GuardSchedule guard_schedule::parse(const GuardScheduleConfig& spec)
{
  return {.enabled = spec.enabled,
          .asleep = parseWindows(spec.asleep),
          .open = parseWindows(spec.open),
          .staffed = parseWindows(spec.staffed),
          .closedMode = spec.closedMode == "armed" ? GuardMode::Armed
                                                   : GuardMode::Away};
}

bool guard_schedule::inWindows(const std::vector<GuardWindow>& windows,
                               const std::tm& local)
{
  const int minute = local.tm_hour * 60 + local.tm_min;
  const int day = local.tm_wday;
  for (const auto& window : windows) {
    if (window.startMinute < window.endMinute) {
      if (dayIn(window.days, day) && minute >= window.startMinute &&
          minute < window.endMinute)
        return true;
      continue;
    }
    if (dayIn(window.days, day) && minute >= window.startMinute)
      return true;
    if (dayIn(window.days, day - 1) && minute < window.endMinute)
      return true;
  }
  return false;
}

GuardPosture guard_schedule::resolve(const GuardPostureInput& input)
{
  const GuardSchedule& schedule = input.schedule;
  if (!schedule.enabled)
    return {.mode = input.manual,
            .publicPresent = false,
            .staffOnly = false,
            .occupancy = "manual"};
  if (input.manual == GuardMode::Armed)
    return {.mode = GuardMode::Armed,
            .publicPresent = false,
            .staffOnly = false,
            .occupancy = "armed"};
  if (inWindows(schedule.open, input.local))
    return {.mode = GuardMode::Home,
            .publicPresent = true,
            .staffOnly = false,
            .occupancy = "open"};
  if (inWindows(schedule.staffed, input.local))
    return {.mode = GuardMode::Home,
            .publicPresent = false,
            .staffOnly = true,
            .occupancy = "staffed"};
  if (!schedule.open.empty() || !schedule.staffed.empty())
    return {.mode = schedule.closedMode,
            .publicPresent = false,
            .staffOnly = false,
            .occupancy = "closed"};
  if (input.manual == GuardMode::Home &&
      inWindows(schedule.asleep, input.local))
    return {.mode = GuardMode::Night,
            .publicPresent = false,
            .staffOnly = false,
            .occupancy = "asleep"};
  return {.mode = input.manual,
          .publicPresent = false,
          .staffOnly = false,
          .occupancy = "manual"};
}

GuardSite guard_schedule::siteDefaults(const GuardServiceConfig& config)
{
  return {.profile =
              siteProfileFromString(config.profile).value_or(SiteProfile::Home),
          .scheduleEnabled = config.schedule.enabled,
          .asleep = config.schedule.asleep,
          .open = config.schedule.open,
          .staffed = config.schedule.staffed,
          .closedMode = config.schedule.closedMode == "armed" ? GuardMode::Armed
                                                              : GuardMode::Away,
          .digestHour = config.digestHour,
          .updatedAt = 0};
}

GuardSchedule guard_schedule::fromSite(const GuardSite& site)
{
  return parse({.enabled = site.scheduleEnabled,
                .asleep = site.asleep,
                .open = site.open,
                .staffed = site.staffed,
                .closedMode = site.closedMode == GuardMode::Armed ? "armed"
                                                                  : "away"});
}
