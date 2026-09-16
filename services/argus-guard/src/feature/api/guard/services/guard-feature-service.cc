#include "guard-feature-service.hxx"

#include <ctime>
#include <fstream>
#include <guard-belief.hxx>
#include <identity/identity-client.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

GuardFeatureService::GuardFeatureService(IdentityClient* identity)
    : identity_(identity)
{
}

drogon::Task<bool> GuardFeatureService::promotePerson(
    const PromotePersonInput& input) const
{
  if (identity_ == nullptr || input.personId <= 0 ||
      input.accessToken.empty())
    co_return false;
  co_return co_await BlockingTask<bool>(
      [this, input]() {
        return identity_->promotePerson(
            {.personId = input.personId,
             .accessToken = input.accessToken,
             .deviceHash = input.deviceHash});
      });
}

drogon::Task<std::string> GuardFeatureService::mode() const
{
  co_return co_await repository_.state("mode", "home");
}

drogon::Task<std::string> GuardFeatureService::setMode(
    const std::string& mode) const
{
  const std::string normalized = guardModeToString(guardModeFromString(mode));
  co_await repository_.setState(
      {.key = "mode",
       .value = normalized,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
  co_return normalized;
}

drogon::Task<Json::Value> GuardFeatureService::incidents(int limit) const
{
  const auto rows = co_await repository_.recentIncidents(limit);
  Json::Value response(Json::arrayValue);
  for (const auto& row : rows)
    response.append(row);
  co_return response;
}

namespace
{
Json::Value detectionHealth()
{
  Json::Value health(Json::objectValue);
  double thermalC = -1.0;
  for (int zone = 0; zone < 8; ++zone) {
    std::ifstream reading("/sys/class/thermal/thermal_zone" +
                          std::to_string(zone) + "/temp");
    int64_t milliC = 0;
    if ((reading >> milliC) && milliC > 0) {
      thermalC = static_cast<double>(milliC) / 1000.0;
      break;
    }
  }
  health["thermalC"] = thermalC;
  health["thermalBucket"] = thermalC < 0       ? "unknown"
                            : thermalC < 70.0  ? "nominal"
                            : thermalC < 80.0  ? "warm"
                                               : "hot";
  return health;
}

std::string holdReasonPhrase(const std::string& reason, bool didNotify)
{
  if (didNotify)
    return "notified";
  if (reason == "thread_suppressed")
    return "held: same-tier repeat";
  if (reason == "belief_gate")
    return "held: belief below threshold";
  if (reason == "budget")
    return "held: action budget";
  if (reason == "staging")
    return "held: early-watch staging";
  return "held: below notify tier";
}

std::string decisionSummaryText(const DecisionJournalRow& row)
{
  const Json::Value signals = json_util::fromString(row.beliefSignals);
  std::string who;
  std::vector<std::string> reasons;
  if (signals.isArray()) {
    for (const auto& signal : signals) {
      if (!signal.isString())
        continue;
      const std::string name = signal.asString();
      if (who.empty() &&
          (name == "identity_known" || name == "identity_unrecognized" ||
           name == "identity_unobservable"))
        who = ::beliefSignalPhrase(name);
      else if (reasons.size() < 3) {
        const std::string phrase = ::beliefSignalPhrase(name);
        if (!phrase.empty())
          reasons.push_back(phrase);
      }
    }
  }
  std::string text = row.severity + " event, camera " +
                     std::to_string(row.cameraId);
  if (!who.empty())
    text += ": " + who;
  if (!reasons.empty()) {
    text += " (";
    for (size_t index = 0; index < reasons.size(); ++index) {
      if (index > 0)
        text += ", ";
      text += reasons[index];
    }
    text += ")";
  }
  text += ". Score " + std::to_string(row.beliefScore) + " vs threshold " +
          std::to_string(row.beliefThreshold) + " — " +
          holdReasonPhrase(row.suppressionReason, row.didNotify) + ".";
  return text;
}

Json::Value decisionEntry(const DecisionJournalRow& row)
{
  Json::Value entry(Json::objectValue);
  entry["eventId"] = row.eventId;
  entry["encounterId"] = Json::Int64(row.encounterId);
  entry["incidentId"] = Json::Int64(row.incidentId);
  entry["cameraId"] = Json::Int64(row.cameraId);
  entry["observationId"] = row.observationId;
  entry["severity"] = row.severity;
  entry["severityRank"] = row.severityRank;
  entry["hardFloor"] = row.hardFloor;
  entry["beliefScore"] = row.beliefScore;
  entry["beliefSignals"] = json_util::fromString(row.beliefSignals);
  entry["beliefThreshold"] = row.beliefThreshold;
  entry["legacyWouldNotify"] = row.legacyWouldNotify;
  entry["beliefWouldNotify"] = row.beliefWouldNotify;
  entry["didNotify"] = row.didNotify;
  entry["decisionMode"] = row.decisionMode;
  entry["suppressionReason"] = row.suppressionReason;
  entry["suppressedKinds"] = json_util::fromString(row.suppressedKinds);
  entry["dispatchAttempts"] = row.dispatchAttempts;
  entry["noveltyScore"] = row.noveltyScore;
  entry["repeatVisits"] = row.repeatVisits;
  entry["quietHold"] = row.quietHold;
  entry["budgetHold"] = row.budgetHold;
  entry["assessMs"] = row.assessMs;
  entry["feedbackLabel"] = row.feedbackLabel;
  entry["feedbackAt"] = Json::Int64(row.feedbackAt);
  entry["summary"] = decisionSummaryText(row);
  entry["createdAt"] = Json::Int64(row.createdAt);
  return entry;
}
} // namespace

drogon::Task<Json::Value> GuardFeatureService::decisions(
    const ListDecisionsDto& query) const
{
  DecisionsFilterInput filter;
  filter.limit = query.limit;
  filter.from = query.from;
  filter.to = query.to;
  filter.cameraId = query.cameraId;
  filter.severity = query.severity;
  filter.decisionMode = query.decisionMode;
  filter.suppressionReason = query.suppressionReason;
  filter.divergentOnly = query.divergentOnly;
  filter.nearMissMargin = query.nearMissMargin;
  filter.afterCreatedAt = query.afterCreatedAt;
  filter.afterEventId = query.afterEventId;
  const DecisionsPage page = co_await repository_.listDecisionsFiltered(filter);
  Json::Value response(Json::objectValue);
  Json::Value rows(Json::arrayValue);
  for (const auto& row : page.rows)
    rows.append(decisionEntry(row));
  response["rows"] = std::move(rows);
  response["hasMore"] = page.hasMore;
  if (page.hasMore) {
    Json::Value cursor(Json::objectValue);
    cursor["createdAt"] = Json::Int64(page.nextCreatedAt);
    cursor["eventId"] = page.nextEventId;
    response["nextCursor"] = std::move(cursor);
  }
  else {
    response["nextCursor"] = Json::Value::null;
  }
  co_return response;
}

drogon::Task<Json::Value> GuardFeatureService::decisionsSummary(
    const DecisionsSummaryInput& input) const
{
  const DecisionSummary summary = co_await repository_.summarizeDecisions(input);
  Json::Value response(Json::objectValue);
  response["totalRows"] = Json::Int64(summary.totalRows);
  response["fired"] = Json::Int64(summary.fired);
  response["legacyWould"] = Json::Int64(summary.legacyWould);
  response["beliefWould"] = Json::Int64(summary.beliefWould);
  response["since"] = Json::Int64(summary.since);
  response["until"] = Json::Int64(summary.until);
  const auto appendGroups = [](Json::Value& out,
                               const std::vector<DecisionSummaryCount>& groups) {
    for (const auto& group : groups) {
      Json::Value entry(Json::objectValue);
      entry["key"] = group.key;
      entry["rows"] = Json::Int64(group.rows);
      entry["fired"] = Json::Int64(group.fired);
      out.append(std::move(entry));
    }
  };
  Json::Value bySeverity(Json::arrayValue);
  appendGroups(bySeverity, summary.bySeverity);
  response["bySeverity"] = std::move(bySeverity);
  Json::Value byReason(Json::arrayValue);
  appendGroups(byReason, summary.byReason);
  response["byReason"] = std::move(byReason);
  Json::Value byMode(Json::arrayValue);
  appendGroups(byMode, summary.byMode);
  response["byMode"] = std::move(byMode);
  Json::Value histogram(Json::arrayValue);
  for (const auto& bucket : summary.scoreHistogram) {
    Json::Value entry(Json::objectValue);
    entry["severity"] = bucket.severity;
    entry["score"] = bucket.score;
    entry["rows"] = Json::Int64(bucket.rows);
    entry["fired"] = Json::Int64(bucket.fired);
    entry["beliefWould"] = Json::Int64(bucket.beliefWould);
    histogram.append(std::move(entry));
  }
  response["scoreHistogram"] = std::move(histogram);
  const auto appendCameraBuckets = [](Json::Value& out,
                                      const std::vector<DecisionCameraBucket>&
                                          buckets) {
    for (const auto& bucket : buckets) {
      Json::Value entry(Json::objectValue);
      entry["cameraId"] = Json::Int64(bucket.cameraId);
      entry["bucket"] = bucket.bucket;
      entry["events"] = Json::Int64(bucket.events);
      entry["notified"] = Json::Int64(bucket.notified);
      out.append(std::move(entry));
    }
  };
  Json::Value byCameraDay(Json::arrayValue);
  appendCameraBuckets(byCameraDay, summary.byCameraDay);
  response["byCameraDay"] = std::move(byCameraDay);
  Json::Value byCameraHour(Json::arrayValue);
  appendCameraBuckets(byCameraHour, summary.byCameraHour);
  response["byCameraHour"] = std::move(byCameraHour);
  Json::Value signals(Json::arrayValue);
  for (const auto& signal : summary.signals) {
    Json::Value entry(Json::objectValue);
    entry["signal"] = signal.signal;
    entry["count"] = Json::Int64(signal.count);
    signals.append(std::move(entry));
  }
  response["signals"] = std::move(signals);
  response["unparseableSignalRows"] =
      Json::Int64(summary.unparseableSignalRows);
  response["ambiguousNotifications"] =
      Json::Int64(summary.ambiguousNotifications);
  response["nearMisses"] = Json::Int64(summary.nearMisses);
  response["quietHeld"] = Json::Int64(summary.quietHeld);
  response["budgetHeld"] = Json::Int64(summary.budgetHeld);
  response["assessMsP50"] = Json::Int64(summary.assessMsP50);
  response["assessMsP95"] = Json::Int64(summary.assessMsP95);
  response["detectionHealth"] =
      co_await BlockingTask<Json::Value>([]() { return detectionHealth(); });
  co_return response;
}

drogon::Task<bool> GuardFeatureService::setFeedback(
    const std::string& eventId, const std::string& label) const
{
  if (!feedbackLabelFromString(label).has_value())
    co_return false;
  co_return co_await repository_.setDecisionFeedback(
      {.eventId = eventId,
       .label = label,
       .at = static_cast<int64_t>(std::time(nullptr))});
}

drogon::Task<int64_t> GuardFeatureService::createGuest(
    const CreateExpectedGuestDto& input) const
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t validFrom = input.validFrom > 0 ? input.validFrom : now;
  const int64_t validUntil = input.validUntil > 0
                                 ? input.validUntil
                                 : now + static_cast<int64_t>(input.hours) * 3600;
  co_return co_await repository_.insertGuest(
      {.description = input.description,
       .cameraId = input.cameraId,
       .personId = input.personId,
       .hostUserId = input.hostUserId,
       .oneTime = input.oneTime,
       .validFrom = validFrom,
       .validUntil = validUntil});
}

drogon::Task<Json::Value> GuardFeatureService::guests() const
{
  const auto rows = co_await repository_.listGuests();
  Json::Value response(Json::arrayValue);
  for (const auto& guest : rows) {
    Json::Value row;
    row["id"] = Json::Int64(guest.id);
    row["description"] = guest.description;
    row["cameraId"] = Json::Int64(guest.cameraId);
    row["personId"] = Json::Int64(guest.personId);
    row["hostUserId"] = Json::Int64(guest.hostUserId);
    row["oneTime"] = guest.oneTime;
    row["validFrom"] = Json::Int64(guest.validFrom);
    row["validUntil"] = Json::Int64(guest.validUntil);
    response.append(row);
  }
  co_return response;
}

drogon::Task<bool> GuardFeatureService::removeGuest(int64_t id) const
{
  co_return co_await repository_.removeGuest(id);
}
