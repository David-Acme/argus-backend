#include "push-outcome-service.hxx"

#include <text/json-util.hxx>

#include <json/value.h>
#include <trantor/utils/Logger.h>

std::optional<PushOutcomeReport> parsePushOutcome(std::string_view payload)
{
  const Json::Value json = json_util::fromString(std::string(payload));
  if (!json.isObject() || !json["outcome"].isString())
    return std::nullopt;
  const auto outcome = pushOutcomeFromString(json["outcome"].asString());
  if (!outcome)
    return std::nullopt;
  PushOutcomeReport report;
  report.outcome = *outcome;
  if (json["token"].isString())
    report.token = json["token"].asString();
  return report;
}

drogon::Task<bool> PushOutcomeService::apply(std::string payload) const
{
  const auto report = parsePushOutcome(payload);
  if (!report) {
    LOG_WARN << "Push result: malformed result dropped";
    co_return false;
  }
  if (report->outcome != PushOutcome::InvalidToken &&
      report->outcome != PushOutcome::Unregistered)
    co_return false;
  if (report->token.empty())
    co_return false;
  const int64_t removed = co_await repository_.removeByToken(report->token);
  if (removed > 0)
    LOG_INFO << "Push result: " << std::string(pushOutcomeToString(report->outcome))
             << "; the device token was removed";
  co_return removed > 0;
}
