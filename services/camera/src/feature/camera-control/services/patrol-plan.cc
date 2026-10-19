#include "patrol-plan.hxx"

#include <algorithm>
#include <string>
#include <utility>

namespace
{
bool heldByOthers(const PatrolSituation& situation)
{
  const PtzHolder holder = situation.lease.holder;
  if (holder == PtzHolder::Viewer || holder == PtzHolder::Person)
    return true;
  if (holder == PtzHolder::Argus)
    return !situation.progress.active;
  return situation.nowMs < situation.lease.parkUntilMs;
}

std::optional<std::string> idText(const Json::Value& id)
{
  if (id.isString())
    return id.asString();
  if (id.isIntegral())
    return std::to_string(id.asInt64());
  return std::nullopt;
}

const Json::Value* idList(const Json::Value& presets)
{
  if (!presets.isObject())
    return nullptr;
  const Json::Value& outer = presets["preset"];
  if (!outer.isObject())
    return nullptr;
  if (outer["id"].isArray())
    return &outer["id"];
  const Json::Value& inner = outer["preset"];
  if (inner.isObject() && inner["id"].isArray())
    return &inner["id"];
  return nullptr;
}
}

namespace patrol_plan
{
bool inWindow(const std::optional<PatrolWindow>& window, int hour)
{
  if (!window || window->startHour == window->endHour)
    return true;
  if (window->startHour < window->endHour)
    return hour >= window->startHour && hour < window->endHour;
  return hour >= window->startHour || hour < window->endHour;
}

PatrolStep decide(const PatrolSituation& situation)
{
  const PatrolProgress& progress = situation.progress;
  const bool runnable = situation.enabled && situation.stopCount > 0 && situation.online &&
                        !situation.masked && situation.moduleActive &&
                        inWindow(situation.window, situation.localHour);
  if (!runnable) {
    if (!progress.active)
      return {.kind = PatrolStepKind::Idle, .stop = 0, .returnHome = false};
    return {.kind = PatrolStepKind::End,
            .stop = 0,
            .returnHome = situation.online && !situation.masked && situation.moduleActive};
  }
  const std::size_t next = progress.nextStop % situation.stopCount;
  if (heldByOthers(situation))
    return {.kind = PatrolStepKind::Yield, .stop = next, .returnHome = false};
  if (progress.active && situation.nowMs < progress.arrivedAtMs + situation.dwellMs)
    return {.kind = PatrolStepKind::Wait, .stop = next, .returnHome = false};
  return {.kind = PatrolStepKind::Advance, .stop = next, .returnHome = false};
}

std::vector<std::string> savedPresetIds(const Json::Value& presets)
{
  std::vector<std::string> ids;
  const Json::Value* list = idList(presets);
  if (list == nullptr)
    return ids;
  ids.reserve(list->size());
  for (const Json::Value& id : *list)
    if (auto text = idText(id))
      ids.push_back(std::move(*text));
  return ids;
}

std::vector<std::string> unknownStops(const PatrolStopCheck& check)
{
  std::vector<std::string> unknown;
  for (const std::string& stop : check.stops) {
    const bool saved = std::ranges::find(check.saved, stop) != check.saved.end();
    const bool listed = std::ranges::find(unknown, stop) != unknown.end();
    if (!saved && !listed)
      unknown.push_back(stop);
  }
  return unknown;
}
}
