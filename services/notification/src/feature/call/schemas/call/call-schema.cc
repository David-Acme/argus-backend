#include "call-schema.hxx"

#include <text/json-util.hxx>

#include <charconv>
#include <string_view>

std::string CallSchema::callId() const
{
  return call_id::format(id);
}

CallSchema CallSchema::fromRow(const drogon::orm::Row& row)
{
  CallSchema call;
  call.id = row["id"].as<int64_t>();
  call.userId = row["user_id"].as<int64_t>();
  call.dedupeKey = row["dedupe_key"].as<std::string>();
  call.trigger = callTriggerFromString(row["trigger"].as<std::string>())
                     .value_or(CallTrigger::Assistant);
  call.state = callStateFromString(row["state"].as<std::string>())
                   .value_or(CallState::Missed);
  call.reason = row["reason"].as<std::string>();
  call.parentCallId = row["parent_call_id"].as<int64_t>();
  call.urgency = row["urgency"].as<std::string>();
  call.lang = row["lang"].as<std::string>();
  call.title = row["title"].as<std::string>();
  call.summary = row["summary"].as<std::string>();
  call.openingLine = row["opening_line"].as<std::string>();
  call.missedLine = row["missed_line"].as<std::string>();
  call.data = json_util::fromString(row["data"].as<std::string>());
  call.answeredSession = row["answered_session"].as<std::string>();
  call.createdAt = row["created_at"].as<int64_t>();
  call.expiresAt = row["expires_at"].as<int64_t>();
  call.pushedAt = row["pushed_at"].as<int64_t>();
  call.answeredAt = row["answered_at"].as<int64_t>();
  call.endedAt = row["ended_at"].as<int64_t>();
  return call;
}

std::string call_id::format(int64_t id)
{
  return "call-" + std::to_string(id);
}

int64_t call_id::parse(const std::string& callId)
{
  constexpr std::string_view kPrefix = "call-";
  if (callId.size() <= kPrefix.size() || callId.size() > kPrefix.size() + 19 ||
      !callId.starts_with(kPrefix))
    return 0;
  int64_t id = 0;
  const char* first = callId.data() + kPrefix.size();
  const char* last = callId.data() + callId.size();
  const auto [end, error] = std::from_chars(first, last, id);
  if (error != std::errc{} || end != last || id <= 0)
    return 0;
  return id;
}
