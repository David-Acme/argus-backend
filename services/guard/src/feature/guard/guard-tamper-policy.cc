#include "guard-tamper-policy.hxx"

#include <algorithm>
#include <map>

namespace guard_tamper
{
bool indicating(std::string_view status, bool sudden)
{
  if (status == "blurred")
    return sudden;
  return status == "moved" || status == "covered" || status == "whiteout" || status == "blinded" ||
         status == "partial_cover" || status == "dark_sudden" || status == "unreachable";
}

TamperWindow windowFor(const TamperWindowInput& input)
{
  const TamperWindows& windows = input.windows;
  const std::string_view status = input.status;
  if (status == "covered")
    return {.action = TamperAction::Alarm, .afterS = input.protect ? windows.coverS : input.homeS};
  if (status == "whiteout")
    return {.action = TamperAction::Alarm, .afterS = input.protect ? windows.whiteoutS : input.homeS};
  if (status == "blinded")
    return {.action = TamperAction::Alarm, .afterS = input.protect ? windows.blindS : windows.blindHomeS};
  if (status == "partial_cover")
    return {.action = TamperAction::Alarm, .afterS = input.protect ? windows.partialS : input.homeS};
  if (status == "moved")
    return {.action = TamperAction::Alarm, .afterS = input.protect ? windows.reaimS : input.homeS};
  if (status == "dark_sudden")
    return input.protect ? TamperWindow{.action = TamperAction::Alarm, .afterS = windows.darkS}
                         : TamperWindow{.action = TamperAction::Ignore, .afterS = 0};
  if (status == "unreachable") {
    if (!input.protect)
      return {.action = TamperAction::Alarm, .afterS = input.offlineHomeS};
    return {.action = TamperAction::Alarm,
            .afterS = input.link == "wired" ? windows.signalWiredS : windows.signalWifiS};
  }
  if (status == "blurred") {
    if (input.sudden && input.protect)
      return {.action = TamperAction::Alarm, .afterS = windows.defocusS};
    return {.action = TamperAction::LensNotice, .afterS = windows.lensNoticeS};
  }
  return {.action = TamperAction::Ignore, .afterS = 0};
}

std::vector<TamperGroup> groupsOf(std::span<const TamperReadingRef> readings, int64_t windowS)
{
  std::map<int64_t, std::vector<TamperReadingRef>> byEnvironment;
  for (const TamperReadingRef& reading : readings)
    byEnvironment[reading.environmentId].push_back(reading);
  std::vector<TamperGroup> groups;
  for (auto& [environmentId, members] : byEnvironment) {
    std::ranges::sort(members, {}, &TamperReadingRef::onsetAt);
    size_t start = 0;
    while (start < members.size()) {
      size_t end = start + 1;
      while (end < members.size() && members.at(end).onsetAt - members.at(start).onsetAt <= windowS)
        ++end;
      if (end - start >= 2) {
        TamperGroup group{.environmentId = environmentId, .cameraIds = {}, .firstOnset = members.at(start).onsetAt};
        for (size_t index = start; index < end; ++index)
          group.cameraIds.push_back(members.at(index).cameraId);
        std::ranges::sort(group.cameraIds);
        groups.push_back(std::move(group));
      }
      start = end;
    }
  }
  return groups;
}

GuardDanger groupDanger(const GroupDangerInput& input)
{
  if (input.protect || !input.anyoneHome)
    return GuardDanger::Critical;
  return GuardDanger::High;
}

SeenVerdict seenVerdict(const SeenInput& input)
{
  if (!input.found)
    return SeenVerdict::None;
  if (input.role == UserRole::Guest)
    return SeenVerdict::GuestNamed;
  const bool household = input.role == UserRole::Owner || input.role == UserRole::Resident ||
                         (input.role == UserRole::Guard && input.onDuty);
  if (!household)
    return SeenVerdict::None;
  if (!input.protect)
    return SeenVerdict::HomeNotice;
  return input.personAway ? SeenVerdict::KeptCritical : SeenVerdict::Lowered;
}

bool shouldAsk(const AskInput& input)
{
  return !input.protect && input.seen == SeenVerdict::None && input.anyoneHome;
}

bool approachingLens(const SightingShareInput& input)
{
  return input.lastShare >= input.nearShare && input.lastShare >= input.firstShare;
}
}
