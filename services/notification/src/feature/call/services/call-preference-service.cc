#include "call-preference-service.hxx"

#include <ctime>

namespace
{
std::optional<CallMode> modeOf(const std::optional<std::string>& value)
{
  if (!value)
    return std::nullopt;
  return callModeFromString(*value);
}
}

drogon::Task<CallPreferenceSchema> CallPreferenceService::read(int64_t userId) const
{
  const auto found = co_await repository_.find(userId);
  co_return found.value_or(CallPreferenceSchema::defaultsFor(userId));
}

drogon::Task<CallPreferenceSchema>
CallPreferenceService::update(int64_t userId,
                              const UpdateCallPreferenceDto& dto) const
{
  co_return co_await repository_.update(
      {.userId = userId,
       .enabled = dto.enabled,
       .guardCritical = modeOf(dto.guardCritical),
       .guardIntruder = modeOf(dto.guardIntruder),
       .guardEscalation = modeOf(dto.guardEscalation),
       .guardArrival = modeOf(dto.guardArrival),
       .agenda = modeOf(dto.agenda),
       .assistant = modeOf(dto.assistant),
       .quietStartHour = dto.quietStartHour,
       .quietEndHour = dto.quietEndHour,
       .dndUntil = dto.dndUntil,
       .criticalBypass = dto.criticalBypass,
       .mutedEnvironmentIds = dto.mutedEnvironmentIds,
       .agendaLeadMinutes = dto.agendaLeadMinutes,
       .quietDays = dto.quietDays,
       .ringSeconds = dto.ringSeconds,
       .pushDelaySeconds = dto.pushDelaySeconds,
       .liveAnnounce = dto.liveAnnounce,
       .lang = dto.lang,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
}
