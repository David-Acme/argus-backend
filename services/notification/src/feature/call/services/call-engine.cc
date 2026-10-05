#include "call-engine.hxx"

#include <feature/call/services/response-copy.hxx>
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
constexpr int64_t kResponseLifetimeS = 2 * kHourS;
constexpr int64_t kClosedVisibleS = 24 * kHourS;
constexpr int kResponseBatch = 50;
constexpr int kResponsesShown = 20;

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
  if (const int64_t responseId = number(call.data, "responseId"); responseId > 0)
    info["responseId"] = static_cast<Json::Int64>(responseId);
  const Json::Value& discreet = call.data["discreet"];
  info["discreet"] = discreet.isBool() && discreet.asBool();
  if (call.data["offers"].isArray())
    info["offers"] = call.data["offers"];
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

CallLocalTime CallEngine::localAt(int64_t at) const
{
  if (dependencies_.localTime)
    return dependencies_.localTime(at);
  const auto seconds = static_cast<std::time_t>(at);
  std::tm local{};
  localtime_r(&seconds, &local);
  return {.hour = local.tm_hour, .weekday = local.tm_wday};
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
    const auto member = std::ranges::find(request.members, userId,
                                          &CallResponseMember::userId);
    try {
      outcomes.push_back(co_await considerUser(
          {.request = request,
           .user = user,
           .config = config,
           .now = at,
           .member = member != request.members.end() ? &*member : nullptr}));
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
  const CallLocalTime clock = localAt(input.now);
  const CallVerdict verdict = call_policy::decide(
      {.trigger = candidate.trigger,
       .critical = candidate.critical,
       .preference = user.preference,
       .localHour = clock.hour,
       .localWeekday = clock.weekday,
       .now = input.now,
       .environmentId = candidate.environmentId,
       .alreadyCalled = alreadyCalled,
       .ringing = ringing.has_value(),
       .lastCallAt = stats.lastAt,
       .callsLastHour = stats.recent,
       .limits = {.enabled = input.config.enabled,
                  .callGapS = input.config.callGapS,
                  .maxCallsPerHour = input.config.maxCallsPerHour},
       .planNotify = input.member != nullptr &&
                     input.member->mode == ResponseMemberMode::Notify,
       .mandatory = input.member != nullptr && input.member->mandatory});
  outcome.reason = verdict.reason;

  const std::string lang =
      langFor(!user.preference.lang.empty() ? user.preference.lang
              : !user.recipient.lang.empty() ? user.recipient.lang
                                             : candidate.lang);
  Json::Value stored = candidate.data.isObject()
                           ? candidate.data
                           : Json::Value(Json::objectValue);
  if (input.member != nullptr && input.member->discreet)
    stored["discreet"] = true;
  const CallCopy copy = call_copy::render({.trigger = candidate.trigger,
                                           .lang = lang,
                                           .data = stored,
                                           .userName = user.recipient.name,
                                           .now = input.now});
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
                      .expiresAt =
                          input.now + user.preference.ringSecondsClamped(),
                      .pushAfter =
                          input.now + user.preference.pushDelayClamped()};

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
                      .endedAt = 0,
                      .pushAfter = row.pushAfter};
      co_await emit({.userId = userId,
                     .operation = SyncOperation::CallIncoming,
                     .info = call_engine::incomingInfo(call)});
      if (row.pushAfter <= input.now &&
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
  info["claimedBy"] = request.sessionId;
  co_await emit({.userId = request.userId,
                 .operation = SyncOperation::CallCancel,
                 .info = info});
  LOG_INFO << "Call engine: " << claimed.callId() << " answered by user "
           << request.userId;
  co_await attendAnswered(claimed);
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
    if (call.pushedAt == 0 && call.pushAfter <= at &&
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
  report.escalated = co_await advanceResponses(at);
  report.expired = static_cast<int64_t>(
      (co_await responseRepository_.expire(at - kResponseLifetimeS, at)).size());
  co_return report;
}

drogon::Task<AgendaAnnouncementOutcome>
CallEngine::announceAgenda(const AgendaAnnouncement& announcement) const
{
  AgendaAnnouncementOutcome outcome;
  std::vector<int64_t> userIds;
  for (const int64_t userId : announcement.userIds) {
    if (userId > 0 && std::ranges::find(userIds, userId) == userIds.end())
      userIds.push_back(userId);
  }
  if (userIds.empty() || announcement.commandId.empty())
    co_return outcome;
  const bool reminder = text(announcement.data, "kind") == "agenda_reminder";
  const auto preferences = co_await preferenceRepository_.findMany(userIds);
  std::vector<int64_t> matched;
  for (const int64_t userId : userIds) {
    auto preference = CallPreferenceSchema::defaultsFor(userId);
    if (const auto found = preferences.find(userId); found != preferences.end())
      preference = found->second;
    if (preference.agenda == CallMode::Off)
      continue;
    const int wanted = reminder ? 0 : preference.agendaLeadMinutes;
    if (wanted != announcement.leadMinutes)
      continue;
    matched.push_back(userId);
    if (!dependencies_.notifier)
      continue;
    if (co_await dependencies_.notifier->notify(
            {.userId = userId,
             .type = "agenda",
             .title = announcement.title,
             .body = announcement.body,
             .data = announcement.data,
             .commandId = announcement.commandId + ":" + std::to_string(userId)}))
      ++outcome.notified;
  }
  if (matched.empty())
    co_return outcome;
  for (const auto& user : co_await considerNotification(announcement.data, matched)) {
    if (user.resolution == CallResolution::Rang)
      ++outcome.rang;
  }
  co_return outcome;
}

namespace
{
std::string placeOf(const Json::Value& data)
{
  std::string camera = text(data, "cameraName");
  std::string environment = text(data, "environmentName");
  if (camera.empty())
    return environment;
  if (!environment.empty())
    camera += " (" + environment + ")";
  return camera;
}

std::vector<CallResponseMember> unreached(const std::vector<CallResponseMember>& members)
{
  std::vector<CallResponseMember> pending;
  for (const auto& member : members) {
    if (member.reachedAt == 0)
      pending.push_back(member);
  }
  return pending;
}
}

drogon::Task<std::vector<CallUserOutcome>>
CallEngine::respond(const ResponseRequest& request) const
{
  const auto parsed = call_response::parsePlan(request.plan);
  if (!parsed)
    co_return co_await considerNotification(request.data, request.userIds);
  auto candidate = call_trigger::fromNotification(request.data);
  if (!candidate)
    co_return std::vector<CallUserOutcome>{};

  const int64_t actor = number(request.data, "actorUserId");
  std::vector<int64_t> userIds;
  for (const int64_t userId : request.userIds) {
    if (userId > 0 && userId != actor &&
        std::ranges::find(userIds, userId) == userIds.end())
      userIds.push_back(userId);
  }
  if (actor > 0 && text(candidate->data, "actorName").empty()) {
    const CallRecipient person = co_await lookupRecipient(actor);
    if (person.found)
      candidate->data["actorName"] = person.name;
  }

  const int64_t at = now();
  const std::string kind = text(request.data, "kind");
  std::vector<CallResponseMemberInput> members;
  members.reserve(parsed->entries.size());
  bool firstStepRings = false;
  for (const auto& entry : parsed->entries) {
    if (entry.userId == actor)
      continue;
    members.push_back({.userId = entry.userId,
                       .step = entry.step,
                       .mode = entry.mode,
                       .mandatory = entry.mandatory,
                       .discreet = entry.discreet});
    firstStepRings = firstStepRings ||
                     (entry.step == 0 && entry.mode == ResponseMemberMode::Call);
  }
  const int64_t planEnvironment = number(request.plan, "environmentId");
  const CallResponseOpened opened = co_await responseRepository_.open(
      {.dedupeKey = candidate->dedupeKey,
       .kind = kind,
       .environmentId = planEnvironment > 0 ? planEnvironment : candidate->environmentId,
       .episodeId = kind == "guard_episode" ? number(request.data, "episodeId") : 0,
       .cameraId = number(request.data, "cameraId"),
       .strategy = parsed->strategy,
       .stepCount = parsed->stepCount,
       .stepSeconds = parsed->stepSeconds,
       .stepDeadline = at + (firstStepRings ? parsed->stepSeconds : 0),
       .plan = json_util::toString(request.plan),
       .data = json_util::toString(candidate->data),
       .at = at},
      members);
  if (!opened.response || !responseOpen(opened.response->state))
    co_return std::vector<CallUserOutcome>{};
  const CallResponseSchema& response = *opened.response;
  candidate->data["responseId"] = static_cast<Json::Int64>(response.id);
  if (response.plan["offers"].isArray())
    candidate->data["offers"] = response.plan["offers"];

  const auto stored = co_await responseRepository_.members(response.id);
  if (!opened.created && candidate->trigger == CallTrigger::GuardEscalation &&
      (response.state == ResponseState::Active ||
       response.state == ResponseState::Unanswered)) {
    std::vector<CallResponseMember> others;
    for (const auto& member : unreached(stored)) {
      if (std::ranges::find(userIds, member.userId) == userIds.end())
        others.push_back(member);
    }
    co_await reach({.response = response,
                    .members = std::move(others),
                    .critical = candidate->critical,
                    .reason = ResponseReach::Worse,
                    .now = at});
  }
  for (const int64_t userId : userIds)
    co_await responseRepository_.markReached(
        {.responseId = response.id, .userId = userId, .at = at});

  auto outcomes = co_await consider({.candidate = *candidate,
                                     .userIds = userIds,
                                     .notificationExists = true,
                                     .members = stored});
  co_await emitResponse(response.id);
  co_return outcomes;
}

drogon::Task<void> CallEngine::reach(const ReachInput& input) const
{
  const CallResponseSchema& response = input.response;
  auto candidate = call_trigger::fromNotification(response.data);
  if (!candidate || input.members.empty())
    co_return;
  candidate->data["responseId"] = static_cast<Json::Int64>(response.id);
  if (response.plan["offers"].isArray())
    candidate->data["offers"] = response.plan["offers"];
  if (input.critical) {
    candidate->critical = true;
    candidate->urgency = "critical";
  }
  std::vector<int64_t> reached;
  reached.reserve(input.members.size());
  for (const auto& member : input.members) {
    if (!co_await responseRepository_.markReached(
            {.responseId = response.id, .userId = member.userId, .at = input.now}))
      continue;
    reached.push_back(member.userId);
    if (!dependencies_.notifier)
      continue;
    const CallRecipient recipient = co_await lookupRecipient(member.userId);
    const std::string lang = langFor(recipient.lang);
    const CallCopy copy = call_copy::render({.trigger = candidate->trigger,
                                             .lang = lang,
                                             .data = candidate->data,
                                             .userName = recipient.name,
                                             .now = input.now});
    Json::Value data = candidate->data;
    data["threadKey"] = response.dedupeKey;
    data["urgency"] = candidate->urgency;
    data["lang"] = lang;
    co_await dependencies_.notifier->notify(
        {.userId = member.userId,
         .type = "camera",
         .title = copy.title,
         .body = response_copy::escalationBody({.lang = lang,
                                                .summary = copy.summary,
                                                .reason = input.reason,
                                                .confirmedBy = input.response.verdictByName}),
         .data = data,
         .commandId = "response:" + std::to_string(response.id) + ":reach:" +
                      std::to_string(member.userId)});
  }
  if (reached.empty())
    co_return;
  co_await consider({.candidate = *candidate,
                     .userIds = reached,
                     .notificationExists = true,
                     .members = co_await responseRepository_.members(response.id)});
}

drogon::Task<void> CallEngine::cancelRinging(const CancelInput& input) const
{
  const auto cancelled = co_await callRepository_.cancelRingingForKey(
      {.dedupeKey = input.response.dedupeKey,
       .exceptUserId = input.exceptUserId,
       .onlyUserId = input.onlyUserId,
       .reason = input.reason,
       .now = input.now});
  for (const auto& call : cancelled) {
    co_await callRepository_.settleFollowups(
        {.parentId = call.id, .state = CallState::Missed, .now = input.now});
    Json::Value info(Json::objectValue);
    info["callId"] = call_id::format(call.id);
    info["reason"] = input.reason;
    info["attendedBy"] = input.attendedBy;
    info["responseId"] = static_cast<Json::Int64>(input.response.id);
    co_await emit({.userId = call.userId,
                   .operation = SyncOperation::CallCancel,
                   .info = info});
  }
}

drogon::Task<void> CallEngine::promptContacts(const PromptInput& input) const
{
  if (!dependencies_.notifier)
    co_return;
  const CallResponseSchema& response = input.response;
  const std::string place = placeOf(response.data);
  for (const auto& member : co_await responseRepository_.members(response.id)) {
    if (member.reachedAt == 0)
      continue;
    const CallRecipient recipient = co_await lookupRecipient(member.userId);
    const std::string lang = langFor(recipient.lang);
    const ResponseNotice notice = response_copy::contacts(
        {.lang = lang,
         .place = place,
         .contacts = response.plan["contacts"],
         .emergencyNumber = text(response.plan, "emergencyNumber"),
         .confirmed = input.confirmed,
         .confirmedBy = response.verdictByName});
    Json::Value data(Json::objectValue);
    data["kind"] = "guard_response";
    data["responseId"] = static_cast<Json::Int64>(response.id);
    data["threadKey"] = response.dedupeKey + ":response";
    data["urgency"] = "time_sensitive";
    data["state"] = responseStateToString(response.state);
    data["environmentId"] = static_cast<Json::Int64>(response.environmentId);
    data["cameraId"] = static_cast<Json::Int64>(response.cameraId);
    data["lang"] = lang;
    co_await dependencies_.notifier->notify(
        {.userId = member.userId,
         .type = "camera",
         .title = notice.title,
         .body = notice.body,
         .data = data,
         .commandId = "response:" + std::to_string(response.id) + ":" +
                      (input.confirmed ? "confirmed" : "unanswered") + ":" +
                      std::to_string(member.userId)});
  }
}

drogon::Task<void> CallEngine::emitResponse(int64_t responseId) const
{
  const auto response = co_await responseRepository_.findById(responseId);
  if (!response)
    co_return;
  for (const auto& member : co_await responseRepository_.members(responseId)) {
    if (member.reachedAt == 0)
      continue;
    co_await emit({.userId = member.userId,
                   .operation = SyncOperation::ResponseUpdate,
                   .info = call_response::toJson({.response = *response, .member = &member})});
  }
}

drogon::Task<void> CallEngine::attendAnswered(const CallSchema& call) const
{
  const auto response = co_await responseRepository_.findByKey(call.dedupeKey);
  if (!response || !responseOpen(response->state))
    co_return;
  const CallRecipient recipient = co_await lookupRecipient(call.userId);
  const int64_t at = now();
  if (!co_await responseRepository_.attend(
          {.id = response->id, .userId = call.userId, .name = recipient.name, .at = at}))
    co_return;
  co_await cancelRinging({.response = *response,
                          .exceptUserId = call.userId,
                          .onlyUserId = 0,
                          .reason = "attended",
                          .attendedBy = recipient.name,
                          .now = at});
  co_await emitResponse(response->id);
  LOG_INFO << "Call engine: " << call_response::responseId(response->id)
           << " attended by user " << call.userId;
}

drogon::Task<int64_t> CallEngine::advanceResponses(int64_t now) const
{
  int64_t advanced = 0;
  for (const auto& response :
       co_await responseRepository_.due({.now = now, .limit = kResponseBatch})) {
    const int next = response.step + 1;
    if (next < response.stepCount) {
      std::vector<CallResponseMember> stepMembers;
      bool rings = false;
      for (const auto& member : co_await responseRepository_.members(response.id)) {
        if (member.step != next || member.reachedAt != 0)
          continue;
        rings = rings || member.mode == ResponseMemberMode::Call;
        stepMembers.push_back(member);
      }
      if (!co_await responseRepository_.advance(
              {.id = response.id,
               .fromStep = response.step,
               .toStep = next,
               .deadline = now + (rings ? response.stepSeconds : 0),
               .at = now}))
        continue;
      co_await reach({.response = response,
                      .members = std::move(stepMembers),
                      .critical = false,
                      .reason = ResponseReach::NextStep,
                      .now = now});
    }
    else {
      if (!co_await responseRepository_.markUnanswered(response.id, now))
        continue;
      if (const auto fresh = co_await responseRepository_.findById(response.id))
        co_await promptContacts({.response = *fresh, .confirmed = false, .now = now});
    }
    co_await emitResponse(response.id);
    ++advanced;
  }
  co_return advanced;
}

drogon::Task<ResponseVerdictOutcome>
CallEngine::verdict(const ResponseVerdictRequest& request) const
{
  ResponseVerdictOutcome outcome;
  const auto member =
      co_await responseRepository_.member(request.responseId, request.userId);
  if (!member || member->reachedAt == 0)
    co_return outcome;
  const CallRecipient recipient = co_await lookupRecipient(request.userId);
  const int64_t at = now();
  const bool recorded = co_await responseRepository_.verdict(
      {.id = request.responseId,
       .verdict = request.verdict,
       .userId = request.userId,
       .name = recipient.name,
       .at = at});
  auto fresh = co_await responseRepository_.findById(request.responseId);
  if (!fresh)
    co_return outcome;
  if (!recorded) {
    outcome.status = fresh->verdict == request.verdict ? ResponseVerdictStatus::Recorded
                                                       : ResponseVerdictStatus::Closed;
    outcome.response = call_response::toJson({.response = *fresh, .member = &*member});
    co_return outcome;
  }
  if (request.verdict == ResponseVerdict::FalseAlarm) {
    co_await cancelRinging({.response = *fresh,
                            .exceptUserId = 0,
                            .onlyUserId = 0,
                            .reason = "resolved",
                            .attendedBy = recipient.name,
                            .now = at});
  }
  else {
    co_await cancelRinging({.response = *fresh,
                            .exceptUserId = 0,
                            .onlyUserId = request.userId,
                            .reason = "attended",
                            .attendedBy = recipient.name,
                            .now = at});
    co_await reach({.response = *fresh,
                    .members = unreached(co_await responseRepository_.members(fresh->id)),
                    .critical = true,
                    .reason = ResponseReach::Confirmed,
                    .now = at});
    co_await promptContacts({.response = *fresh, .confirmed = true, .now = at});
  }
  if (dependencies_.verdicts)
    dependencies_.verdicts->publish({.responseId = fresh->id,
                                     .kind = fresh->kind,
                                     .threadKey = fresh->dedupeKey,
                                     .episodeId = fresh->episodeId,
                                     .environmentId = fresh->environmentId,
                                     .verdict = responseVerdictToString(request.verdict),
                                     .userId = request.userId,
                                     .at = at});
  co_await emitResponse(fresh->id);
  LOG_INFO << "Call engine: " << call_response::responseId(fresh->id) << " marked "
           << responseVerdictToString(request.verdict) << " by user " << request.userId;
  outcome.status = ResponseVerdictStatus::Recorded;
  outcome.response = call_response::toJson({.response = *fresh, .member = &*member});
  co_return outcome;
}

drogon::Task<Json::Value> CallEngine::responses(int64_t userId) const
{
  Json::Value list(Json::arrayValue);
  const int64_t at = now();
  for (const auto& response : co_await responseRepository_.forUser(
           {.userId = userId, .closedSince = at - kClosedVisibleS, .limit = kResponsesShown})) {
    const auto member = co_await responseRepository_.member(response.id, userId);
    list.append(call_response::toJson(
        {.response = response, .member = member ? &*member : nullptr}));
  }
  co_return list;
}

drogon::Task<std::optional<Json::Value>>
CallEngine::response(const ResponseViewRequest& request) const
{
  const auto member =
      co_await responseRepository_.member(request.responseId, request.userId);
  if (!member || member->reachedAt == 0)
    co_return std::nullopt;
  const auto found = co_await responseRepository_.findById(request.responseId);
  if (!found)
    co_return std::nullopt;
  co_return call_response::toJson({.response = *found, .member = &*member});
}
