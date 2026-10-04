#include "call-engine.hxx"

#include <runtime/blocking-task.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <ctime>
#include <utility>

namespace
{
constexpr int64_t kHourS = 3600;
constexpr std::size_t kTopicLimit = 300;
constexpr int kDueBatch = 50;
constexpr const char* kMissedTitleKey = "callMissedTitle";

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

std::string deepLink(const std::string& callId)
{
  return "argus://call?callId=" + callId;
}

Json::Value missedData(const CallSchema& call)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "call";
  data["callId"] = call.callId();
  data["threadKey"] = call.dedupeKey;
  data["urgency"] = call.urgency;
  data["summary"] = call.missedLine;
  data["trigger"] = callTriggerToString(call.trigger);
  data["callKind"] = std::string(callKindOf(call.trigger));
  data["lang"] = call.lang;
  if (const int64_t episodeId = number(call.data, "episodeId"); episodeId > 0)
    data["episodeId"] = static_cast<Json::Int64>(episodeId);
  if (const int64_t cameraId = number(call.data, "cameraId"); cameraId > 0)
    data["cameraId"] = static_cast<Json::Int64>(cameraId);
  return data;
}
}

std::string callResolutionToString(CallResolution resolution)
{
  switch (resolution) {
    case CallResolution::Rang:
      return "rang";
    case CallResolution::Queued:
      return "queued";
    case CallResolution::Injected:
      return "injected";
    case CallResolution::Notified:
      return "notified";
    case CallResolution::Dropped:
      return "dropped";
  }
  return "dropped";
}

Json::Value call_engine::incomingInfo(const CallSchema& call)
{
  Json::Value info(Json::objectValue);
  info["callId"] = call.callId();
  info["reason"] = call.title;
  info["summary"] = call.summary;
  info["urgency"] = call.urgency;
  info["kind"] = std::string(callKindOf(call.trigger));
  info["lang"] = call.lang;
  info["expiresAt"] = static_cast<Json::Int64>(call.expiresAt);
  if (const int64_t episodeId = number(call.data, "episodeId"); episodeId > 0)
    info["episodeId"] = static_cast<Json::Int64>(episodeId);
  if (const int64_t cameraId = number(call.data, "cameraId"); cameraId > 0)
    info["cameraId"] = static_cast<Json::Int64>(cameraId);
  if (const std::string camera = text(call.data, "cameraName"); !camera.empty())
    info["cameraName"] = camera;
  if (const std::string environment = text(call.data, "environmentName");
      !environment.empty())
    info["environmentName"] = environment;
  return info;
}

CallEngine::CallEngine(CallEngineConfig config,
                       CallEngineDependencies dependencies)
    : config_(std::move(config)), dependencies_(std::move(dependencies))
{
}

void CallEngine::reconfigure(const CallEngineConfig& config)
{
  const std::scoped_lock lock(configMutex_);
  config_ = config;
}

CallEngineConfig CallEngine::config() const
{
  const std::scoped_lock lock(configMutex_);
  return config_;
}

int64_t CallEngine::now() const
{
  if (dependencies_.clock)
    return dependencies_.clock();
  return static_cast<int64_t>(std::time(nullptr));
}

int CallEngine::hourAt(int64_t at) const
{
  if (dependencies_.localHour)
    return dependencies_.localHour(at);
  const auto seconds = static_cast<std::time_t>(at);
  std::tm local{};
  localtime_r(&seconds, &local);
  return local.tm_hour;
}

std::string CallEngine::langFor(const std::string& preferred) const
{
  if (preferred == "es" || preferred == "en")
    return preferred;
  return std::string(call_copy::normalizeLang(config().fallbackLang));
}

drogon::Task<CallRecipient> CallEngine::lookupRecipient(int64_t userId) const
{
  if (!dependencies_.directory)
    co_return CallRecipient{};
  if (!dependencies_.blockingOffLoop)
    co_return dependencies_.directory->recipient(userId);
  const auto directory = dependencies_.directory;
  co_return co_await BlockingTask<CallRecipient>(
      [directory, userId]() { return directory->recipient(userId); });
}

drogon::Task<CallPerson> CallEngine::lookupPerson(int64_t personId) const
{
  if (!dependencies_.directory)
    co_return CallPerson{};
  if (!dependencies_.blockingOffLoop)
    co_return dependencies_.directory->person(personId);
  const auto directory = dependencies_.directory;
  co_return co_await BlockingTask<CallPerson>(
      [directory, personId]() { return directory->person(personId); });
}

drogon::Task<std::optional<bool>>
CallEngine::announce(const CallAnnouncement& announcement) const
{
  if (!dependencies_.announcer)
    co_return std::nullopt;
  if (!dependencies_.blockingOffLoop)
    co_return dependencies_.announcer->announce(announcement);
  const auto announcer = dependencies_.announcer;
  co_return co_await BlockingTask<std::optional<bool>>(
      [announcer, announcement]() { return announcer->announce(announcement); });
}

drogon::Task<bool> CallEngine::emit(const CallSignalInput& input) const
{
  if (!dependencies_.signal)
    co_return false;
  if (!dependencies_.blockingOffLoop)
    co_return dependencies_.signal->emit(input);
  const auto signal = dependencies_.signal;
  co_return co_await BlockingTask<bool>(
      [signal, input]() { return signal->emit(input); });
}

void CallEngine::pushRing(const CallSchema& call) const
{
  if (!dependencies_.push)
    return;
  Json::Value data(Json::objectValue);
  data["kind"] = "call";
  data["callId"] = call.callId();
  data["urgency"] = call.urgency;
  data["callKind"] = std::string(callKindOf(call.trigger));
  data["deepLink"] = deepLink(call.callId());
  data["expiresAt"] = static_cast<Json::Int64>(call.expiresAt);
  dependencies_.push->publish(PushIntent{.userId = call.userId,
                                         .notificationId = 0,
                                         .type = "call",
                                         .title = call.title,
                                         .body = call.summary,
                                         .createdAtMs = call.createdAt * 1000,
                                         .data = data});
}

drogon::Task<std::vector<CallUserOutcome>>
CallEngine::considerNotification(const Json::Value& data,
                                 const std::vector<int64_t>& userIds) const
{
  const auto candidate = call_trigger::fromNotification(data);
  if (!candidate)
    co_return std::vector<CallUserOutcome>{};
  co_return co_await consider({.candidate = *candidate,
                               .userIds = userIds,
                               .notificationExists = true});
}

drogon::Task<std::vector<CallUserOutcome>>
CallEngine::consider(const CallRequest& request) const
{
  std::vector<CallUserOutcome> outcomes;
  std::vector<int64_t> userIds;
  for (const int64_t userId : request.userIds) {
    if (userId > 0 && std::ranges::find(userIds, userId) == userIds.end())
      userIds.push_back(userId);
  }
  if (userIds.empty() || request.candidate.dedupeKey.empty())
    co_return outcomes;

  const CallEngineConfig config = this->config();
  const int64_t at = now();
  const auto preferences = co_await preferenceRepository_.findMany(userIds);
  outcomes.reserve(userIds.size());
  for (const int64_t userId : userIds) {
    UserContext user{.userId = userId,
                     .recipient = co_await lookupRecipient(userId),
                     .preference = CallPreferenceSchema::defaultsFor(userId)};
    if (const auto found = preferences.find(userId); found != preferences.end())
      user.preference = found->second;
    try {
      outcomes.push_back(co_await considerUser(
          {.request = request, .user = user, .config = config, .now = at}));
    }
    catch (const std::exception& error) {
      LOG_WARN << "Call engine: user " << userId << " for "
               << request.candidate.dedupeKey << " failed: " << error.what();
      outcomes.push_back({.userId = userId,
                          .resolution = CallResolution::Dropped,
                          .reason = "error",
                          .callId = 0});
    }
  }
  co_return outcomes;
}

drogon::Task<CallUserOutcome>
CallEngine::considerUser(const ConsiderUserInput& input) const
{
  const CallCandidate& candidate = input.request.candidate;
  const UserContext& user = input.user;
  const int64_t userId = user.userId;
  CallUserOutcome outcome{.userId = userId,
                          .resolution = CallResolution::Dropped,
                          .reason = "",
                          .callId = 0};
  if (user.recipient.found && !user.recipient.active) {
    outcome.reason = "inactive";
    co_return outcome;
  }

  const bool alreadyCalled =
      co_await callRepository_.exists(candidate.dedupeKey, userId);
  const auto ringing = co_await callRepository_.findRingingFor(
      {.userId = userId, .now = input.now});
  const CallRingStats stats = co_await callRepository_.ringStats(
      {.userId = userId, .since = input.now - kHourS});
  const CallVerdict verdict = call_policy::decide(
      {.trigger = candidate.trigger,
       .critical = candidate.critical,
       .preference = user.preference,
       .localHour = hourAt(input.now),
       .now = input.now,
       .environmentId = candidate.environmentId,
       .alreadyCalled = alreadyCalled,
       .ringing = ringing.has_value(),
       .lastCallAt = stats.lastAt,
       .callsLastHour = stats.recent,
       .limits = {.enabled = input.config.enabled,
                  .callGapS = input.config.callGapS,
                  .maxCallsPerHour = input.config.maxCallsPerHour}});
  outcome.reason = verdict.reason;

  const std::string lang =
      langFor(!user.recipient.lang.empty() ? user.recipient.lang : candidate.lang);
  const CallCopy copy = call_copy::render({.trigger = candidate.trigger,
                                           .lang = lang,
                                           .data = candidate.data,
                                           .userName = user.recipient.name,
                                           .now = input.now});
  Json::Value stored = candidate.data.isObject()
                           ? candidate.data
                           : Json::Value(Json::objectValue);
  stored[kMissedTitleKey] = copy.missedTitle;
  CallCreateInput row{.userId = userId,
                      .dedupeKey = candidate.dedupeKey,
                      .trigger = candidate.trigger,
                      .state = CallState::Ringing,
                      .reason = verdict.reason,
                      .parentCallId = 0,
                      .urgency = candidate.urgency.empty() ? "active"
                                                           : candidate.urgency,
                      .lang = lang,
                      .title = copy.title,
                      .summary = copy.summary,
                      .openingLine = copy.openingLine,
                      .missedLine = copy.missedLine,
                      .data = json_util::toString(stored),
                      .createdAt = input.now,
                      .expiresAt = input.now + input.config.ringTimeoutS};

  const bool askedReminder = candidate.trigger == CallTrigger::Assistant &&
                             verdict.reason == "trigger_off";
  if (verdict.decision == CallDecision::Drop && !askedReminder)
    co_return outcome;

  if (verdict.injectable) {
    const auto delivered = co_await announce(
        {.userId = userId,
         .text = copy.followupLine,
         .kind = std::string(callKindOf(candidate.trigger)),
         .callId = ""});
    if (delivered.value_or(false)) {
      row.state = CallState::Injected;
      row.reason = "live_call";
      row.expiresAt = input.now;
      const auto id = co_await callRepository_.create(row);
      outcome.resolution = CallResolution::Injected;
      outcome.reason = "live_call";
      outcome.callId = id.value_or(0);
      co_return outcome;
    }
  }

  if (verdict.decision == CallDecision::Notify || askedReminder) {
    outcome.resolution = CallResolution::Notified;
    if (!input.request.notificationExists && dependencies_.notifier) {
      Json::Value data = candidate.data.isObject()
                             ? candidate.data
                             : Json::Value(Json::objectValue);
      data["threadKey"] = candidate.dedupeKey;
      data["urgency"] = "passive";
      data["lang"] = lang;
      co_await dependencies_.notifier->notify(
          {.userId = userId,
           .type = candidate.trigger == CallTrigger::GuardArrival
                        ? "camera"
                        : "reminder",
           .title = copy.title,
           .body = copy.summary,
           .data = data,
           .commandId = "call-notice:" + candidate.dedupeKey + ":" +
                        std::to_string(userId)});
    }
    co_return outcome;
  }

  if (verdict.decision == CallDecision::Ring) {
    if (const auto id = co_await callRepository_.createRinging(row)) {
      CallSchema call{.id = *id,
                      .userId = userId,
                      .dedupeKey = row.dedupeKey,
                      .trigger = row.trigger,
                      .state = CallState::Ringing,
                      .reason = row.reason,
                      .parentCallId = 0,
                      .urgency = row.urgency,
                      .lang = row.lang,
                      .title = row.title,
                      .summary = row.summary,
                      .openingLine = row.openingLine,
                      .missedLine = row.missedLine,
                      .data = stored,
                      .answeredSession = "",
                      .createdAt = row.createdAt,
                      .expiresAt = row.expiresAt,
                      .pushedAt = 0,
                      .answeredAt = 0,
                      .endedAt = 0};
      co_await emit({.userId = userId,
                     .operation = SyncOperation::CallIncoming,
                     .info = call_engine::incomingInfo(call)});
      if (input.config.inAppGraceS <= 0 &&
          co_await callRepository_.markPushed(call.id, input.now))
        pushRing(call);
      outcome.resolution = CallResolution::Rang;
      outcome.callId = *id;
      LOG_INFO << "Call engine: ringing user " << userId << " ("
               << callTriggerToString(candidate.trigger) << ", "
               << call.callId() << ")";
      co_return outcome;
    }
    if (co_await callRepository_.exists(candidate.dedupeKey, userId)) {
      outcome.reason = "already_called";
      co_return outcome;
    }
  }

  const auto parent = ringing ? ringing
                              : co_await callRepository_.findRingingFor(
                                    {.userId = userId, .now = input.now});
  if (!parent) {
    outcome.resolution = CallResolution::Dropped;
    outcome.reason = "ring_race";
    co_return outcome;
  }
  row.state = CallState::Queued;
  row.parentCallId = parent->id;
  row.reason = "ringing";
  row.expiresAt = parent->expiresAt;
  const auto id = co_await callRepository_.create(row);
  outcome.resolution = id ? CallResolution::Queued : CallResolution::Dropped;
  outcome.reason = id ? "ringing" : "already_called";
  outcome.callId = id.value_or(0);
  co_return outcome;
}

drogon::Task<CallClaimOutcome>
CallEngine::claim(const CallClaimRequest& request) const
{
  CallClaimOutcome outcome;
  const int64_t id = call_id::parse(request.callId);
  if (id <= 0 || request.userId <= 0)
    co_return outcome;
  const auto existing = co_await callRepository_.findById(id);
  if (!existing || existing->userId != request.userId ||
      existing->parentCallId != 0 || existing->state == CallState::Injected ||
      existing->state == CallState::Queued)
    co_return outcome;

  const int64_t at = now();
  if (existing->state == CallState::Answered && !request.sessionId.empty() &&
      existing->answeredSession == request.sessionId) {
    outcome.status = CallClaimStatus::Claimed;
    outcome.call = existing;
    outcome.openingLine = existing->openingLine;
    co_return outcome;
  }
  if (existing->state != CallState::Ringing) {
    outcome.status = existing->state == CallState::Missed
                         ? CallClaimStatus::Expired
                         : CallClaimStatus::Taken;
    co_return outcome;
  }
  if (existing->expiresAt <= at) {
    outcome.status = CallClaimStatus::Expired;
    co_return outcome;
  }
  if (!co_await callRepository_.claim({.id = id,
                                       .userId = request.userId,
                                       .sessionId = request.sessionId,
                                       .now = at})) {
    const auto raced = co_await callRepository_.findById(id);
    outcome.status = raced && raced->state == CallState::Missed
                         ? CallClaimStatus::Expired
                         : CallClaimStatus::Taken;
    co_return outcome;
  }

  const auto followups = co_await callRepository_.findFollowups(id);
  std::vector<std::string> lines;
  lines.reserve(followups.size());
  for (const auto& followup : followups) {
    const CallCopy copy = call_copy::render({.trigger = followup.trigger,
                                             .lang = existing->lang,
                                             .data = followup.data,
                                             .userName = "",
                                             .now = at});
    lines.push_back(copy.followupLine);
  }
  if (!followups.empty())
    co_await callRepository_.settleFollowups(
        {.parentId = id, .state = CallState::Injected, .now = at});

  CallSchema claimed = *existing;
  claimed.state = CallState::Answered;
  claimed.answeredAt = at;
  claimed.answeredSession = request.sessionId;
  outcome.status = CallClaimStatus::Claimed;
  outcome.openingLine = call_copy::joinFollowups(existing->openingLine, lines);
  outcome.call = claimed;

  Json::Value info(Json::objectValue);
  info["callId"] = claimed.callId();
  info["reason"] = "answered_elsewhere";
  co_await emit({.userId = request.userId,
                 .operation = SyncOperation::CallCancel,
                 .info = info});
  LOG_INFO << "Call engine: " << claimed.callId() << " answered by user "
           << request.userId;
  co_return outcome;
}

drogon::Task<bool> CallEngine::end(const CallEndRequest& request) const
{
  const int64_t id = call_id::parse(request.callId);
  if (id <= 0 || request.userId <= 0)
    co_return false;
  const auto existing = co_await callRepository_.findById(id);
  if (!existing || existing->userId != request.userId)
    co_return false;
  if (existing->state != CallState::Answered)
    co_return existing->state == CallState::Completed ||
              existing->state == CallState::Declined;

  const bool declined =
      request.outcome == CallEndReport::Declined || !request.spoken;
  const int64_t at = now();
  if (!co_await callRepository_.end(
          {.id = id,
           .userId = request.userId,
           .state = declined ? CallState::Declined : CallState::Completed,
           .now = at}))
    co_return false;
  if (declined && dependencies_.notifier) {
    co_await dependencies_.notifier->notify(
        {.userId = existing->userId,
         .type = "call",
         .title = text(existing->data, kMissedTitleKey),
         .body = existing->missedLine,
         .data = missedData(*existing),
         .commandId = "call-declined:" + existing->callId()});
  }
  co_return true;
}

drogon::Task<void> CallEngine::settleMissed(const MissedInput& input) const
{
  const CallSchema& call = input.call;
  if (!co_await callRepository_.markMissed(call.id, input.now))
    co_return;
  const auto followups = co_await callRepository_.findFollowups(call.id);
  co_await callRepository_.settleFollowups(
      {.parentId = call.id, .state = CallState::Missed, .now = input.now});

  Json::Value info(Json::objectValue);
  info["callId"] = call.callId();
  info["reason"] = input.cancelReason;
  co_await emit({.userId = call.userId,
                 .operation = SyncOperation::CallCancel,
                 .info = info});

  if (!dependencies_.notifier)
    co_return;
  std::vector<std::string> lines;
  lines.reserve(followups.size());
  for (const auto& followup : followups) {
    const CallCopy copy = call_copy::render({.trigger = followup.trigger,
                                             .lang = call.lang,
                                             .data = followup.data,
                                             .userName = "",
                                             .now = input.now});
    lines.push_back(copy.followupLine);
  }
  co_await dependencies_.notifier->notify(
      {.userId = call.userId,
       .type = "call",
       .title = text(call.data, kMissedTitleKey),
       .body = call_copy::joinFollowups(call.missedLine, lines),
       .data = missedData(call),
       .commandId = "call-missed:" + call.callId()});
  LOG_INFO << "Call engine: " << call.callId() << " missed by user "
           << call.userId;
}

drogon::Task<CallScheduleOutcome>
CallEngine::schedule(const CallScheduleRequest& request) const
{
  const CallEngineConfig config = this->config();
  const int64_t at = now();
  CallScheduleOutcome outcome;
  if (request.userId <= 0 || request.commandId.empty() ||
      request.commandId.size() > 128) {
    outcome.reason = "user and command are required";
    co_return outcome;
  }
  if (request.topic.empty() || request.topic.size() > kTopicLimit) {
    outcome.reason = "topic must be 1-300 bytes";
    co_return outcome;
  }
  if (request.fireAt < at - 60 || request.fireAt > at + config.scheduleHorizonS) {
    outcome.reason = "fire_at outside the schedule window";
    co_return outcome;
  }
  if (co_await scheduledRepository_.pendingCount(request.userId) >=
      config.maxPendingScheduled) {
    outcome.status = CallScheduleStatus::TooMany;
    outcome.reason = "too many pending calls";
    co_return outcome;
  }
  const auto created = co_await scheduledRepository_.create(
      {.userId = request.userId,
       .commandId = request.commandId,
       .fireAt = std::max(request.fireAt, at),
       .topic = request.topic,
       .lang = langFor(request.lang),
       .createdAt = at});
  outcome.scheduledId = created.id;
  if (created.conflict) {
    outcome.status = CallScheduleStatus::Conflict;
    outcome.reason = "command id reused with another call";
  }
  else if (created.duplicate) {
    outcome.status = CallScheduleStatus::Duplicate;
  }
  else if (created.id > 0) {
    outcome.status = CallScheduleStatus::Scheduled;
  }
  else {
    outcome.reason = "not stored";
  }
  co_return outcome;
}

drogon::Task<std::vector<CallUserOutcome>>
CallEngine::arrival(const KnownSeenEvent& event) const
{
  std::vector<CallUserOutcome> outcomes;
  if (event.personId <= 0 || event.at <= 0)
    co_return outcomes;
  const CallEngineConfig config = this->config();
  const int64_t previous = co_await arrivalRepository_.touch(
      {.personId = event.personId, .seenAt = event.at});
  if (previous > 0 && event.at - previous < config.arrivalAbsenceS)
    co_return outcomes;
  if (previous > event.at)
    co_return outcomes;

  const auto subscribers = co_await preferenceRepository_.findArrivalSubscribers();
  if (subscribers.empty())
    co_return outcomes;
  const CallPerson person = co_await lookupPerson(event.personId);
  std::vector<int64_t> userIds;
  for (const auto& subscriber : subscribers) {
    if (person.userId > 0 && subscriber.userId == person.userId)
      continue;
    const CallRecipient recipient = co_await lookupRecipient(subscriber.userId);
    if (recipient.found && recipient.role == "guest")
      continue;
    userIds.push_back(subscriber.userId);
  }
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_arrival";
  data["personId"] = static_cast<Json::Int64>(event.personId);
  data["personName"] = person.found ? person.name : std::string{};
  data["cameraId"] = static_cast<Json::Int64>(event.cameraId);
  data["cameraName"] = event.cameraName;
  data["environmentId"] = static_cast<Json::Int64>(event.environmentId);
  data["environmentName"] = event.environmentName;
  co_return co_await consider(
      {.candidate = {.trigger = CallTrigger::GuardArrival,
                     .critical = false,
                     .dedupeKey = "guard:arrival:" +
                                  std::to_string(event.personId) + ":" +
                                  std::to_string(event.at),
                     .urgency = "active",
                     .environmentId = event.environmentId,
                     .lang = "",
                     .data = data},
       .userIds = userIds,
       .notificationExists = false});
}

drogon::Task<CallSweepReport> CallEngine::sweep() const
{
  const CallEngineConfig config = this->config();
  const int64_t at = now();
  CallSweepReport report;
  for (const auto& call : co_await callRepository_.findRinging()) {
    if (call.expiresAt <= at) {
      co_await settleMissed({.call = call, .cancelReason = "expired", .now = at});
      ++report.missed;
      continue;
    }
    if (call.pushedAt == 0 && at - call.createdAt >= config.inAppGraceS &&
        co_await callRepository_.markPushed(call.id, at)) {
      pushRing(call);
      ++report.pushed;
    }
  }

  for (const auto& due :
       co_await scheduledRepository_.due({.now = at, .limit = kDueBatch})) {
    if (!co_await scheduledRepository_.markFired(due.id, at))
      continue;
    ++report.fired;
    Json::Value data(Json::objectValue);
    data["kind"] = "assistant_reminder";
    data["topic"] = due.topic;
    data["scheduledId"] = static_cast<Json::Int64>(due.id);
    data["lang"] = due.lang;
    const std::string dedupeKey = "assistant:scheduled:" + std::to_string(due.id);
    if (at - due.fireAt > config.scheduledLateS) {
      if (dependencies_.notifier) {
        const CallCopy copy = call_copy::render({.trigger = CallTrigger::Assistant,
                                                 .lang = due.lang,
                                                 .data = data,
                                                 .userName = "",
                                                 .now = at});
        data["threadKey"] = dedupeKey;
        data["urgency"] = "active";
        co_await dependencies_.notifier->notify({.userId = due.userId,
                                                 .type = "reminder",
                                                 .title = copy.title,
                                                 .body = copy.summary,
                                                 .data = data,
                                                 .commandId = "call-late:" +
                                                              dedupeKey});
      }
      continue;
    }
    co_await consider({.candidate = {.trigger = CallTrigger::Assistant,
                                     .critical = false,
                                     .dedupeKey = dedupeKey,
                                     .urgency = "time_sensitive",
                                     .environmentId = 0,
                                     .lang = due.lang,
                                     .data = data},
                       .userIds = {due.userId},
                       .notificationExists = false});
  }

  report.closed =
      co_await callRepository_.closeStaleAnswered(at - config.answeredStaleS, at);
  co_return report;
}
