#include "call-trigger-classifier.hxx"

namespace
{
std::string text(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isString() ? value.asString() : std::string{};
}

int64_t number(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isIntegral() ? value.asInt64() : 0;
}

std::optional<CallCandidate> guardCandidate(const Json::Value& data)
{
  const std::string urgency = text(data, "urgency");
  const bool critical = urgency == "critical";
  if (!critical && urgency != "time_sensitive")
    return std::nullopt;
  std::string dedupeKey = text(data, "threadKey");
  if (dedupeKey.empty()) {
    const int64_t episodeId = number(data, "episodeId");
    if (episodeId <= 0)
      return std::nullopt;
    dedupeKey = "guard:episode:" + std::to_string(episodeId);
  }
  CallTrigger trigger = critical ? CallTrigger::GuardCritical
                                 : CallTrigger::GuardIntruder;
  if (text(data, "phase") == "escalated")
    trigger = CallTrigger::GuardEscalation;
  return CallCandidate{.trigger = trigger,
                       .critical = critical,
                       .dedupeKey = dedupeKey,
                       .urgency = urgency,
                       .environmentId = number(data, "environmentId"),
                       .lang = text(data, "lang"),
                       .data = data};
}

std::optional<CallCandidate> agendaCandidate(const Json::Value& data)
{
  const std::string dedupeKey = text(data, "threadKey");
  if (dedupeKey.empty())
    return std::nullopt;
  return CallCandidate{.trigger = CallTrigger::Agenda,
                       .critical = false,
                       .dedupeKey = dedupeKey,
                       .urgency = "time_sensitive",
                       .environmentId = 0,
                       .lang = text(data, "lang"),
                       .data = data};
}
}

std::optional<CallCandidate>
call_trigger::fromNotification(const Json::Value& data)
{
  if (!data.isObject())
    return std::nullopt;
  const std::string kind = text(data, "kind");
  if (kind == "guard_episode")
    return guardCandidate(data);
  if ((kind == "guard_panic" || kind == "guard_duress" || kind == "guard_tamper") &&
      text(data, "urgency") == "critical")
    return guardCandidate(data);
  if (kind == "agenda_event" || kind == "agenda_reminder")
    return agendaCandidate(data);
  return std::nullopt;
}
