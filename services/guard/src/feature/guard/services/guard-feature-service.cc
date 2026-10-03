#include "guard-feature-service.hxx"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <feature/guard/guard-belief.hxx>
#include <identity/identity-client.hxx>
#include <text/json-util.hxx>
#include <runtime/blocking-task.hxx>
#include <feature/guard/vocabulary/feedback-label.hxx>

GuardFeatureService::GuardFeatureService(
    const GuardFeatureDependencies& dependencies)
    : identity_(dependencies.identity), defaultMode_(dependencies.defaultMode),
      siteDefaults_(dependencies.siteDefaults)
{
}

drogon::Task<GuardSite> GuardFeatureService::activeSite() const
{
  if (const auto stored = co_await siteRepository_.find())
    co_return *stored;
  co_return siteDefaults_;
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

drogon::Task<Json::Value> GuardFeatureService::mode() const
{
  const std::string manual =
      co_await guardRepository_.state("mode", guardModeToString(defaultMode_));
  const GuardSite site = co_await activeSite();
  const GuardSchedule schedule = guard_schedule::fromSite(site);
  const auto now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const GuardPosture posture = guard_schedule::resolve(
      {.schedule = schedule, .manual = guardModeFromString(manual),
       .local = local});
  Json::Value response(Json::objectValue);
  response["mode"] = manual;
  response["profile"] = siteProfileToString(site.profile);
  response["effectiveMode"] = guardModeToString(posture.mode);
  response["occupancy"] = posture.occupancy;
  response["publicPresent"] = posture.publicPresent;
  response["staffOnly"] = posture.staffOnly;
  co_return response;
}

drogon::Task<std::string> GuardFeatureService::setMode(
    const std::string& mode) const
{
  const std::string normalized = guardModeToString(guardModeFromString(mode));
  co_await guardRepository_.setState(
      {.key = "mode",
       .value = normalized,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
  co_return normalized;
}

drogon::Task<Json::Value> GuardFeatureService::incidents(int limit) const
{
  const auto rows = co_await guardRepository_.recentIncidents(limit);
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
  entry["encounterId"] = static_cast<Json::Int64>(row.encounterId);
  entry["incidentId"] = static_cast<Json::Int64>(row.incidentId);
  entry["cameraId"] = static_cast<Json::Int64>(row.cameraId);
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
  entry["feedbackAt"] = static_cast<Json::Int64>(row.feedbackAt);
  entry["summary"] = decisionSummaryText(row);
  entry["createdAt"] = static_cast<Json::Int64>(row.createdAt);
  return entry;
}
}

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
  const DecisionsPage page = co_await guardRepository_.listDecisionsFiltered(filter);
  Json::Value response(Json::objectValue);
  Json::Value rows(Json::arrayValue);
  for (const auto& row : page.rows)
    rows.append(decisionEntry(row));
  response["rows"] = std::move(rows);
  response["hasMore"] = page.hasMore;
  if (page.hasMore) {
    Json::Value cursor(Json::objectValue);
    cursor["createdAt"] = static_cast<Json::Int64>(page.nextCreatedAt);
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
  const DecisionSummary summary = co_await guardRepository_.summarizeDecisions(input);
  Json::Value response(Json::objectValue);
  response["totalRows"] = static_cast<Json::Int64>(summary.totalRows);
  response["fired"] = static_cast<Json::Int64>(summary.fired);
  response["legacyWould"] = static_cast<Json::Int64>(summary.legacyWould);
  response["beliefWould"] = static_cast<Json::Int64>(summary.beliefWould);
  response["since"] = static_cast<Json::Int64>(summary.since);
  response["until"] = static_cast<Json::Int64>(summary.until);
  const auto appendGroups = [](Json::Value& out,
                               const std::vector<DecisionSummaryCount>& groups) {
    for (const auto& group : groups) {
      Json::Value entry(Json::objectValue);
      entry["key"] = group.key;
      entry["rows"] = static_cast<Json::Int64>(group.rows);
      entry["fired"] = static_cast<Json::Int64>(group.fired);
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
    entry["rows"] = static_cast<Json::Int64>(bucket.rows);
    entry["fired"] = static_cast<Json::Int64>(bucket.fired);
    entry["beliefWould"] = static_cast<Json::Int64>(bucket.beliefWould);
    histogram.append(std::move(entry));
  }
  response["scoreHistogram"] = std::move(histogram);
  const auto appendCameraBuckets = [](Json::Value& out,
                                      const std::vector<DecisionCameraBucket>&
                                          buckets) {
    for (const auto& bucket : buckets) {
      Json::Value entry(Json::objectValue);
      entry["cameraId"] = static_cast<Json::Int64>(bucket.cameraId);
      entry["bucket"] = bucket.bucket;
      entry["events"] = static_cast<Json::Int64>(bucket.events);
      entry["notified"] = static_cast<Json::Int64>(bucket.notified);
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
    entry["count"] = static_cast<Json::Int64>(signal.count);
    signals.append(std::move(entry));
  }
  response["signals"] = std::move(signals);
  response["unparseableSignalRows"] =
      static_cast<Json::Int64>(summary.unparseableSignalRows);
  response["ambiguousNotifications"] =
      static_cast<Json::Int64>(summary.ambiguousNotifications);
  response["nearMisses"] = static_cast<Json::Int64>(summary.nearMisses);
  response["quietHeld"] = static_cast<Json::Int64>(summary.quietHeld);
  response["budgetHeld"] = static_cast<Json::Int64>(summary.budgetHeld);
  response["assessMsP50"] = static_cast<Json::Int64>(summary.assessMsP50);
  response["assessMsP95"] = static_cast<Json::Int64>(summary.assessMsP95);
  response["detectionHealth"] =
      co_await BlockingTask<Json::Value>([]() { return detectionHealth(); });
  co_return response;
}

drogon::Task<bool> GuardFeatureService::setFeedback(
    const std::string& eventId, const std::string& label) const
{
  if (!feedbackLabelFromString(label).has_value())
    co_return false;
  co_return co_await guardRepository_.setDecisionFeedback(
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
  co_return co_await guardRepository_.insertGuest(
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
  const auto rows = co_await guardRepository_.listGuests();
  Json::Value response(Json::arrayValue);
  for (const auto& guest : rows) {
    Json::Value row;
    row["id"] = static_cast<Json::Int64>(guest.id);
    row["description"] = guest.description;
    row["cameraId"] = static_cast<Json::Int64>(guest.cameraId);
    row["personId"] = static_cast<Json::Int64>(guest.personId);
    row["hostUserId"] = static_cast<Json::Int64>(guest.hostUserId);
    row["oneTime"] = guest.oneTime;
    row["validFrom"] = static_cast<Json::Int64>(guest.validFrom);
    row["validUntil"] = static_cast<Json::Int64>(guest.validUntil);
    response.append(row);
  }
  co_return response;
}

drogon::Task<bool> GuardFeatureService::removeGuest(int64_t id) const
{
  co_return co_await guardRepository_.removeGuest(id);
}

namespace
{
std::string closedModeName(GuardMode mode)
{
  return mode == GuardMode::Armed ? "armed" : "away";
}

Json::Value siteJson(const GuardSite& site)
{
  Json::Value json(Json::objectValue);
  json["profile"] = siteProfileToString(site.profile);
  json["scheduleEnabled"] = site.scheduleEnabled;
  json["asleep"] = site.asleep;
  json["open"] = site.open;
  json["staffed"] = site.staffed;
  json["closedMode"] = closedModeName(site.closedMode);
  json["digestHour"] = site.digestHour;
  json["updatedAt"] = static_cast<Json::Int64>(site.updatedAt);
  return json;
}

Json::Value cameraJson(const GuardCameraContext& camera)
{
  Json::Value json(Json::objectValue);
  json["cameraId"] = static_cast<Json::Int64>(camera.cameraId);
  json["role"] = cameraRoleToString(camera.role);
  json["outdoor"] = camera.outdoor;
  json["publicArea"] = camera.publicArea;
  json["activeHours"] = camera.activeHours;
  json["updatedAt"] = static_cast<Json::Int64>(camera.updatedAt);
  return json;
}

Json::Value reasonsArray(const std::string& stored)
{
  const Json::Value parsed = json_util::fromString(stored);
  return parsed.isArray() ? parsed : Json::Value(Json::arrayValue);
}

Json::Value episodeJson(const EpisodeRow& row)
{
  const bool closed = row.state == "closed";
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id);
  json["kind"] = "person";
  json["cameraId"] = static_cast<Json::Int64>(row.cameraId);
  json["cameraName"] = "";
  json["state"] = closed ? "resolved" : "active";
  json["stage"] = row.state;
  json["danger"] = guardDangerToString(guardDangerFromRank(row.reasonsRank));
  json["notified"] = row.notifyCount > 0;
  json["notifyCount"] = row.notifyCount;
  json["highestNotified"] =
      guardDangerToString(guardDangerFromRank(row.notifyHighestRank));
  json["subject"] = row.subject;
  json["people"] = row.people;
  json["reasons"] = reasonsArray(row.reasons);
  json["firstSeen"] = static_cast<Json::Int64>(row.firstSeen);
  json["lastSeen"] = static_cast<Json::Int64>(row.lastSeen);
  json["observations"] = row.checks;
  json["groupId"] = static_cast<Json::Int64>(row.groupId);
  json["reviewLabel"] = row.reviewLabel;
  json["reviewedAt"] = static_cast<Json::Int64>(row.reviewedAt);
  json["resolution"] = !closed ? ""
                       : row.lastReason == "known_resident" ? "recognized"
                                                            : "left";
  json["spoke"] = row.spoke;
  json["sounded"] = row.sounded;
  json["status"] = "";
  return json;
}

Json::Value tamperJson(const TamperRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.incidentId);
  json["kind"] = "camera";
  json["cameraId"] = static_cast<Json::Int64>(row.cameraId);
  json["cameraName"] = row.cameraName;
  json["state"] = row.open ? "active" : "resolved";
  json["stage"] = row.open ? "degraded" : "closed";
  json["danger"] = row.danger;
  json["notified"] = true;
  json["notifyCount"] = 1;
  json["highestNotified"] = row.danger;
  json["subject"] = "";
  json["people"] = 0;
  json["reasons"] = Json::Value(Json::arrayValue);
  json["firstSeen"] = static_cast<Json::Int64>(row.createdAt);
  json["lastSeen"] = static_cast<Json::Int64>(row.createdAt);
  json["observations"] = 1;
  json["groupId"] = 0;
  json["reviewLabel"] = "";
  json["reviewedAt"] = 0;
  json["resolution"] = row.open ? "" : "recovered";
  json["spoke"] = false;
  json["sounded"] = false;
  json["status"] = row.status;
  return json;
}

Json::Value condensedTimeline(const std::vector<EpisodeTimelineRow>& rows)
{
  constexpr Json::ArrayIndex kMaxEntries = 60;
  Json::Value timeline(Json::arrayValue);
  for (const auto& row : rows) {
    Json::Value* last =
        timeline.empty() ? nullptr : &timeline[timeline.size() - 1];
    if (row.entry == "state") {
      if (last != nullptr && (*last)["type"] == "state" &&
          (*last)["state"] == row.what)
        continue;
      Json::Value entry(Json::objectValue);
      entry["type"] = "state";
      entry["at"] = static_cast<Json::Int64>(row.at);
      entry["state"] = row.what;
      entry["reason"] = row.detail;
      timeline.append(std::move(entry));
      continue;
    }
    if (row.entry == "decision") {
      if (last != nullptr && (*last)["type"] == "decision" &&
          (*last)["danger"] == row.what &&
          (*last)["suppression"] == row.detail &&
          (*last)["notified"].asBool() == row.notified) {
        (*last)["count"] = (*last)["count"].asInt() + 1;
        (*last)["until"] = static_cast<Json::Int64>(row.at);
        continue;
      }
      Json::Value entry(Json::objectValue);
      entry["type"] = "decision";
      entry["at"] = static_cast<Json::Int64>(row.at);
      entry["until"] = static_cast<Json::Int64>(row.at);
      entry["danger"] = row.what;
      entry["suppression"] = row.detail;
      entry["notified"] = row.notified;
      entry["reasons"] = reasonsArray(row.reasons);
      entry["count"] = 1;
      timeline.append(std::move(entry));
      continue;
    }
    Json::Value entry(Json::objectValue);
    entry["type"] = "action";
    entry["at"] = static_cast<Json::Int64>(row.at);
    entry["action"] = row.what;
    entry["status"] = row.detail;
    timeline.append(std::move(entry));
  }
  if (timeline.size() <= kMaxEntries)
    return timeline;
  Json::Value tail(Json::arrayValue);
  for (Json::ArrayIndex index = timeline.size() - kMaxEntries;
       index < timeline.size(); ++index)
    tail.append(timeline[index]);
  return tail;
}
}

drogon::Task<Json::Value> GuardFeatureService::site() const
{
  co_return siteJson(co_await activeSite());
}

drogon::Task<Json::Value>
GuardFeatureService::updateSite(const UpdateGuardSiteDto& input) const
{
  const auto profile = input.profile
                           ? siteProfileFromString(*input.profile)
                           : std::optional<SiteProfile>{};
  const auto closedMode = input.closedMode
                              ? std::optional<GuardMode>(
                                    guardModeFromString(*input.closedMode))
                              : std::optional<GuardMode>{};
  const GuardSite updated = co_await siteRepository_.update(
      {.seed = siteDefaults_,
       .profile = profile,
       .scheduleEnabled = input.scheduleEnabled,
       .asleep = input.asleep,
       .open = input.open,
       .staffed = input.staffed,
       .closedMode = closedMode,
       .digestHour = input.digestHour,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
  co_return siteJson(updated);
}

drogon::Task<Json::Value> GuardFeatureService::cameras() const
{
  const auto contexts = co_await cameraContextRepository_.list();
  Json::Value response(Json::arrayValue);
  for (const auto& context : contexts)
    response.append(cameraJson(context));
  co_return response;
}

drogon::Task<Json::Value>
GuardFeatureService::setCamera(const CameraContextInput& input) const
{
  const GuardCameraContext saved = co_await cameraContextRepository_.upsert(
      {.cameraId = input.cameraId,
       .role = cameraRoleFromString(input.context.role)
                   .value_or(CameraRole::Other),
       .outdoor = input.context.outdoor,
       .publicArea = input.context.publicArea,
       .activeHours = input.context.activeHours,
       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
  co_return cameraJson(saved);
}

drogon::Task<Json::Value>
GuardFeatureService::episodes(const ListEpisodesDto& query) const
{
  const EpisodeListInput window{.limit = query.limit, .before = query.before};
  const auto people = co_await episodeRepository_.list(window);
  const auto cameras = co_await episodeRepository_.tamper(window);
  std::vector<Json::Value> merged;
  merged.reserve(people.size() + cameras.size());
  for (const auto& row : people)
    merged.push_back(episodeJson(row));
  for (const auto& row : cameras)
    merged.push_back(tamperJson(row));
  std::ranges::stable_sort(merged, [](const Json::Value& left,
                                      const Json::Value& right) {
    return left["lastSeen"].asInt64() > right["lastSeen"].asInt64();
  });
  if (merged.size() > static_cast<size_t>(query.limit))
    merged.resize(static_cast<size_t>(query.limit));
  Json::Value rows(Json::arrayValue);
  for (auto& row : merged)
    rows.append(std::move(row));
  Json::Value response(Json::objectValue);
  const bool full = rows.size() == static_cast<Json::ArrayIndex>(query.limit);
  response["nextBefore"] =
      full ? Json::Value(rows[rows.size() - 1]["lastSeen"]) : Json::Value();
  response["rows"] = std::move(rows);
  co_return response;
}

drogon::Task<std::optional<Json::Value>>
GuardFeatureService::episode(int64_t id) const
{
  const auto row = co_await episodeRepository_.find(id);
  if (!row)
    co_return std::nullopt;
  Json::Value response = episodeJson(*row);
  response["timeline"] =
      condensedTimeline(co_await episodeRepository_.timeline(id));
  co_return response;
}

drogon::Task<std::optional<Json::Value>>
GuardFeatureService::reviewEpisode(const ReviewInput& input) const
{
  if (!co_await episodeRepository_.review(
          {.encounterId = input.episodeId,
           .label = input.label,
           .at = static_cast<int64_t>(std::time(nullptr))}))
    co_return std::nullopt;
  const auto row = co_await episodeRepository_.find(input.episodeId);
  if (!row)
    co_return std::nullopt;
  co_return episodeJson(*row);
}
