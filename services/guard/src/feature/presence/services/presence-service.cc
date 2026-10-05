#include "presence-service.hxx"

#include <algorithm>
#include <ctime>
#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/blocking-task.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <utility>

namespace
{
constexpr const char* kLabel = "Presence";

int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}

int64_t integerOf(const Json::Value& event, const char* key)
{
  const Json::Value& value = event[key];
  return value.isIntegral() ? value.asInt64() : 0;
}

int64_t atOf(const Json::Value& event)
{
  const int64_t at = integerOf(event, "at");
  return at > 0 ? at : nowSeconds();
}

class ReleaseOnExit
{
public:
  explicit ReleaseOnExit(std::shared_ptr<std::atomic<int64_t>> counter)
      : counter_(std::move(counter))
  {
  }

  ~ReleaseOnExit() { counter_->fetch_sub(1, std::memory_order_acq_rel); }

  ReleaseOnExit(const ReleaseOnExit&) = delete;
  ReleaseOnExit& operator=(const ReleaseOnExit&) = delete;

private:
  std::shared_ptr<std::atomic<int64_t>> counter_;
};
}

PresenceService::PresenceService(Dependencies dependencies,
                                 GuardPresenceConfig config)
    : dependencies_(std::move(dependencies)), config_(std::move(config))
{
}

PresenceService::~PresenceService()
{
  requestStop();
}

void PresenceService::start()
{
  if (!config_.enabled) {
    LOG_INFO << kLabel << ": disabled by [presence] enabled";
    return;
  }
  alive_.store(true, std::memory_order_release);
  subscribe();
  auto* loop = drogon::app().getLoop();
  timers_.push_back(loop->runEvery(
      static_cast<double>(config_.sweepSeconds), [this]() {
        dispatch("{}", [](PresenceService& self, Json::Value&&) {
          return self.sweep(nowSeconds());
        });
      }));
  timers_.push_back(loop->runEvery(
      static_cast<double>(config_.consentRefreshSeconds), [this]() {
        dispatch("{}", [](PresenceService& self, Json::Value&&) {
          return self.reconcileConsent();
        });
      }));
  dispatch("{}", [](PresenceService& self, Json::Value&&) {
    return self.reconcileConsent();
  });
}

void PresenceService::requestStop()
{
  alive_.store(false, std::memory_order_release);
  if (drogon::app().isRunning())
    for (const uint64_t timer : timers_)
      drogon::app().getLoop()->invalidateTimer(timer);
  timers_.clear();
  unsubscribe();
}

bool PresenceService::drained() const
{
  return inFlight_->load(std::memory_order_acquire) == 0;
}

void PresenceService::refresh(const GuardPresenceConfig& config)
{
  const std::scoped_lock lock(mutex_);
  config_.awayTimeoutSeconds = config.awayTimeoutSeconds;
  config_.tunnelGraceSeconds = config.tunnelGraceSeconds;
  config_.retentionSeconds = config.retentionSeconds;
}

PresenceRules PresenceService::rules() const
{
  const std::scoped_lock lock(mutex_);
  return {.awayTimeoutSeconds = config_.awayTimeoutSeconds,
          .tunnelGraceSeconds = config_.tunnelGraceSeconds,
          .retentionSeconds = config_.retentionSeconds};
}

void PresenceService::subscribe()
{
  NatsBus* bus = dependencies_.bus;
  if (bus == nullptr)
    return;
  const auto listen = [this, bus](const char* subject, Handler handler) {
    const auto id = bus->subscribe(
        subject, [this, handler](std::string_view, std::string_view payload) {
          dispatch(std::string(payload), handler);
        });
    if (id)
      subscriptions_.push_back(*id);
    else
      LOG_WARN << kLabel << ": could not subscribe to " << subject;
  };
  listen(nats_subject::kAuthPresenceSignal,
         [](PresenceService& self, Json::Value&& event) {
           return self.onSessionSignal(std::move(event));
         });
  listen(nats_subject::kGuardKnownSeen,
         [](PresenceService& self, Json::Value&& event) {
           return self.onKnownSeen(std::move(event));
         });
  listen(nats_subject::kIdentityChange,
         [](PresenceService& self, Json::Value&& event) {
           return self.onIdentityChange(std::move(event));
         });
}

void PresenceService::unsubscribe()
{
  if (dependencies_.bus != nullptr)
    for (const uint64_t id : subscriptions_)
      dependencies_.bus->unsubscribe(id);
  subscriptions_.clear();
}

void PresenceService::dispatch(std::string payload, Handler handler)
{
  if (!alive_.load(std::memory_order_acquire))
    return;
  auto counter = inFlight_;
  counter->fetch_add(1, std::memory_order_acq_rel);
  drogon::app().getLoop()->queueInLoop(
      [this, counter, handler, payload = std::move(payload)]() mutable {
        drogon::async_run([this, counter, handler,
                           payload = std::move(payload)]() -> drogon::Task<void> {
          const ReleaseOnExit release(counter);
          try {
            co_await handler(*this, json_util::fromString(payload));
          }
          catch (const std::exception& error) {
            LOG_WARN << kLabel << ": " << error.what();
          }
        });
      });
}

drogon::Task<bool> PresenceService::consented(int64_t userId)
{
  const int64_t now = nowSeconds();
  {
    const std::scoped_lock lock(mutex_);
    const auto cached = consent_.find(userId);
    if (cached != consent_.end() && cached->second.expiresAt > now)
      co_return cached->second.consent;
  }
  const PresenceDirectory* directory = dependencies_.directory;
  if (directory == nullptr)
    co_return false;
  const auto answer = co_await BlockingTask<std::optional<bool>>(
      [directory, userId]() { return directory->presenceConsent(userId); });
  if (!answer)
    co_return false;
  remember(userId, *answer);
  co_return *answer;
}

bool PresenceService::withdrawn(int64_t userId) const
{
  const std::scoped_lock lock(mutex_);
  const auto cached = consent_.find(userId);
  return cached != consent_.end() && !cached->second.consent;
}

void PresenceService::remember(int64_t userId, bool consent)
{
  const std::scoped_lock lock(mutex_);
  if (consent_.size() >= kConsentCacheLimit && !consent_.contains(userId))
    consent_.clear();
  consent_[userId] = {.consent = consent,
                      .expiresAt = nowSeconds() + kConsentCacheSeconds};
}

drogon::Task<void> PresenceService::ingest(PresenceIngestInput input)
{
  if (input.userId <= 0 || !co_await consented(input.userId))
    co_return;
  std::vector<int64_t> environments;
  if (input.environmentId > 0)
    environments.push_back(input.environmentId);
  else
    environments = co_await repository_.lanEnvironments();
  const PresenceRules current = rules();
  for (const int64_t environmentId : environments) {
    const PresenceSignal signal{.userId = input.userId,
                                .environmentId = environmentId,
                                .kind = input.kind,
                                .at = input.at};
    const PresenceDecision decision = co_await repository_.transition(
        {.key = {.userId = input.userId, .environmentId = environmentId},
         .decide = [signal, current](const std::optional<PresenceRow>& existing) {
           const PresenceOutcome outcome = presence_engine::apply(
               {.current = existing, .signal = signal, .rules = current});
           return PresenceDecision{
               .row = outcome.row, .write = outcome.write, .changed = outcome.changed};
         }});
    if (!decision.write)
      continue;
    if (withdrawn(input.userId)) {
      co_await forget(input.userId);
      co_return;
    }
    if (decision.changed)
      co_await announce(decision.row);
  }
}

drogon::Task<void> PresenceService::onSessionSignal(Json::Value event)
{
  const auto kind = presence_engine::kindOf(
      {.origin = sessionOriginFromString(event.get("origin", "").asString()),
       .platform =
           sessionPlatformFromString(event.get("platform", "").asString())});
  if (!kind)
    co_return;
  co_await ingest({.userId = integerOf(event, "userId"),
                   .environmentId = 0,
                   .kind = *kind,
                   .at = atOf(event)});
}

drogon::Task<void> PresenceService::onKnownSeen(Json::Value event)
{
  if (event.get("passerby", false).asBool())
    co_return;
  const int64_t personId = integerOf(event, "personId");
  const int64_t environmentId = integerOf(event, "environmentId");
  const PresenceDirectory* directory = dependencies_.directory;
  if (personId <= 0 || environmentId <= 0 || directory == nullptr)
    co_return;
  const auto userId = co_await BlockingTask<std::optional<int64_t>>(
      [directory, personId]() { return directory->userOfPerson(personId); });
  if (!userId)
    co_return;
  co_await ingest({.userId = *userId,
                   .environmentId = environmentId,
                   .kind = PresenceSignalKind::Camera,
                   .at = atOf(event)});
}

drogon::Task<void> PresenceService::onIdentityChange(Json::Value event)
{
  if (event.get(sync_change::kKindField, "").asString() !=
          sync_change::kKindIdentity ||
      event.get(sync_change::kTableField, "").asString() !=
          tableNameToString(TableName::User))
    co_return;
  const Json::Value& id = event[sync_change::kRecordIdField];
  const int64_t userId = id.isIntegral() ? id.asInt64() : 0;
  const Json::Value& row = event[sync_change::kRowField];
  if (userId <= 0 || !row.isObject())
    co_return;
  if (!row.get("isActive", true).asBool()) {
    co_await forget(userId);
    if (dependencies_.onAccountDisabled)
      co_await dependencies_.onAccountDisabled(userId);
    co_return;
  }
  const Json::Value& privacy = row["privacy"];
  if (!privacy.isObject())
    co_return;
  const bool consent = privacy.get("decided", false).asBool() &&
                       privacy.get("presence", false).asBool();
  if (consent) {
    remember(userId, true);
    co_return;
  }
  co_await forget(userId);
}

drogon::Task<void> PresenceService::forget(int64_t userId)
{
  remember(userId, false);
  const auto environments = co_await repository_.removeUser(userId);
  if (dependencies_.publisher == nullptr)
    co_return;
  const int64_t now = nowSeconds();
  for (const int64_t environmentId : environments)
    dependencies_.publisher->publish(
        {.row = {.userId = userId,
                 .environmentId = environmentId,
                 .state = PresenceState::Unknown,
                 .source = PresenceSource::Consent,
                 .since = now,
                 .lastHomeAt = 0,
                 .lastSignalAt = now},
         .overall = PresenceState::Unknown});
}

drogon::Task<void> PresenceService::announce(PresenceRow row)
{
  if (dependencies_.publisher == nullptr)
    co_return;
  const auto rows = co_await repository_.forUser(row.userId);
  dependencies_.publisher->publish(
      {.row = row, .overall = presence::overall(rows)});
}

drogon::Task<void> PresenceService::sweep(int64_t now)
{
  const PresenceRules current = rules();
  const auto expired = co_await repository_.expireHome(
      {.homeBefore = now - current.awayTimeoutSeconds, .at = now});
  for (const PresenceRow& row : expired)
    co_await announce(row);
  static_cast<void>(
      co_await repository_.purgeStale(now - current.retentionSeconds));
}

drogon::Task<void> PresenceService::reconcileConsent()
{
  const PresenceDirectory* directory = dependencies_.directory;
  if (directory == nullptr)
    co_return;
  const auto consenting =
      co_await BlockingTask<std::optional<std::vector<int64_t>>>(
          [directory]() { return directory->consentingUsers(); });
  if (!consenting)
    co_return;
  const std::unordered_set<int64_t> allowed(consenting->begin(),
                                            consenting->end());
  for (const int64_t userId : allowed)
    remember(userId, true);
  for (const int64_t userId : co_await repository_.userIds())
    if (!allowed.contains(userId))
      co_await forget(userId);
}

drogon::Task<std::vector<PresenceUserView>>
PresenceService::snapshot(std::vector<int64_t> userIds) const
{
  const auto rows = co_await repository_.list();
  std::vector<PresenceUserView> views;
  for (const PresenceRow& row : rows) {
    if (!userIds.empty() && std::ranges::find(userIds, row.userId) ==
                                userIds.end())
      continue;
    if (views.empty() || views.back().userId != row.userId)
      views.push_back({.userId = row.userId,
                       .overall = PresenceState::Unknown,
                       .since = 0,
                       .environments = {}});
    views.back().environments.push_back(row);
  }
  for (PresenceUserView& view : views) {
    view.overall = presence::overall(view.environments);
    for (const PresenceRow& row : view.environments)
      if (row.state == view.overall &&
          (view.since == 0 || row.since < view.since))
        view.since = row.since;
  }
  co_return views;
}
