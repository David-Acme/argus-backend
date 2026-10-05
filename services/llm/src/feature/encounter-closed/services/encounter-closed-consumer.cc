#include "encounter-closed-consumer.hxx"

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <drogon/drogon.h>
#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <text/json-util.hxx>
#include <text/sha256.hxx>
#include <nats/nats-bus.hxx>
#include <trantor/utils/Logger.h>

namespace
{
constexpr int64_t kSettledReceiptRetentionS = 14LL * 24 * 3600;
constexpr int64_t kPurgeIntervalS = 24LL * 3600;
constexpr int64_t kPurgeBatch = 500;
constexpr int64_t kPurgeBacklogIntervalS = 60;
std::string encounterFingerprint(const std::string& payload)
{
  return argus::hash::sha256Hex(
      json_util::toString(json_util::fromString(payload)));
}
}

struct EncounterLifecycle
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
  std::mutex mutex;
  std::condition_variable idle;
};

class EncounterLifecycleGuard
{
public:
  explicit EncounterLifecycleGuard(
      std::shared_ptr<EncounterLifecycle> lifecycle)
      : lifecycle_(std::move(lifecycle))
  {
    if (lifecycle_)
      lifecycle_->active.fetch_add(1, std::memory_order_acq_rel);
  }

  ~EncounterLifecycleGuard()
  {
    if (!lifecycle_)
      return;
    if (lifecycle_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lock(lifecycle_->mutex);
      lifecycle_->idle.notify_all();
    }
  }

  bool alive() const
  {
    return lifecycle_ != nullptr &&
           lifecycle_->alive.load(std::memory_order_acquire);
  }

private:
  std::shared_ptr<EncounterLifecycle> lifecycle_;
};

std::optional<EncounterClosedEvent> EncounterClosedEvent::fromJson(
    const Json::Value& json)
{
  if (!json.isObject())
    return std::nullopt;
  EncounterClosedEvent event;
  event.eventId = json.get("eventId", "").asString();
  event.cameraId = json.get("cameraId", 0).asInt64();
  event.personId = json.get("personId", 0).asInt64();
  event.grade = json.get("grade", "").asString();
  event.durationS = json.get("durationS", 0).asInt64();
  event.closedAt = json.get("closedAt", 0).asInt64();
  if (event.eventId.empty() || event.cameraId <= 0)
    return std::nullopt;
  return event;
}

EncounterClosedConsumer::EncounterClosedConsumer(Dependencies dependencies,
                                                 Config config)
    : dependencies_(std::move(dependencies)), config_(std::move(config)),
      lifecycle_(std::make_shared<EncounterLifecycle>())
{
}

EncounterClosedConsumer::~EncounterClosedConsumer()
{
  stop();
}

void EncounterClosedConsumer::start()
{
  if (dependencies_.bus == nullptr)
    return;
  if (trySubscribe())
    return;
  LOG_WARN << "Encounter consumer: stream not ready; retrying";
  scheduleSubscribeRetry();
}

void EncounterClosedConsumer::stop()
{
  lifecycle_->alive.store(false, std::memory_order_release);
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  if (subscription_.has_value() && dependencies_.bus != nullptr)
    dependencies_.bus->unsubscribe(*subscription_);
  subscription_.reset();
  std::unique_lock lock(lifecycle_->mutex);
  lifecycle_->idle.wait(lock, [lifecycle = lifecycle_] {
    return lifecycle->active.load(std::memory_order_acquire) == 0;
  });
}

void EncounterClosedConsumer::requestStop()
{
  lifecycle_->alive.store(false, std::memory_order_release);
}

bool EncounterClosedConsumer::drained() const
{
  return lifecycle_->active.load(std::memory_order_acquire) == 0;
}

drogon::Task<EncounterDisposition>
EncounterClosedConsumer::handlePayload(const std::string& payload)
{
  co_return settlePayload(payload);
}

drogon::Task<EncounterDisposition> EncounterClosedConsumer::handle(
    const EncounterClosedEvent& event, const std::string& fingerprint)
{
  co_return settleEvent(event, fingerprint);
}

EncounterDisposition EncounterClosedConsumer::settlePayload(const std::string& payload)
{
  const EncounterLifecycleGuard guard(lifecycle_);
  if (!guard.alive())
    return EncounterDisposition::Nak;
  const Json::Value json = json_util::fromString(payload);
  const auto event = EncounterClosedEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Encounter consumer: dropped malformed payload";
    return EncounterDisposition::Term;
  }
  return settleEvent(*event, encounterFingerprint(payload));
}

EncounterDisposition EncounterClosedConsumer::settleEvent(
    const EncounterClosedEvent& event, const std::string& fingerprint)
{
  if (dependencies_.graph == nullptr || dependencies_.repository == nullptr ||
      !dependencies_.capture) {
    LOG_ERROR << "Encounter consumer: dependencies missing; redelivering";
    return EncounterDisposition::Nak;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  EncounterClosedClaim claim{.duplicate = true};
  {
    std::scoped_lock lock(dependencies_.graph->mutex());
    claim = dependencies_.repository->claimEncounterClosed(
        dependencies_.graph->handle(),
        {.eventId = event.eventId, .fingerprint = fingerprint, .at = now});
  }
  if (claim.duplicate)
    return EncounterDisposition::Ack;
  int64_t episode = 0;
  std::string captureError;
  try {
    episode = dependencies_.capture({.cameraId = event.cameraId,
                                      .personId = event.personId,
                                      .grade = event.grade,
                                      .durationS = event.durationS,
                                      .closedAt = event.closedAt,
                                      .ownerUserId = config_.ownerUserId,
                                      .lang = config_.lang});
  }
  catch (const std::exception& error) {
    captureError = error.what();
  }
  const bool captured = captureError.empty() && episode > 0;
  if (!captured) {
    int64_t attempts = -1;
    {
      std::scoped_lock lock(dependencies_.graph->mutex());
      attempts = dependencies_.repository->noteEncounterAttempt(
          dependencies_.graph->handle(), {.eventId = event.eventId, .at = now});
    }
    LOG_WARN << "Encounter consumer: capture failed for " << event.eventId
             << " (attempt " << attempts
             << (captureError.empty() ? "; rejected" : ": " + captureError)
             << ")";
    if (attempts >= 0 && attempts >= config_.poisonMaxAttempts) {
      std::scoped_lock lock(dependencies_.graph->mutex());
      if (dependencies_.repository->markEncounterDeadLettered(
              dependencies_.graph->handle(),
              {.eventId = event.eventId, .at = now})) {
        LOG_ERROR << "Encounter consumer: dead-lettered poison encounter "
                  << event.eventId;
        return EncounterDisposition::Term;
      }
    }
    return EncounterDisposition::Nak;
  }
  {
    std::scoped_lock lock(dependencies_.graph->mutex());
    if (!dependencies_.repository->markEncounterDispatched(
            dependencies_.graph->handle(),
            {.eventId = event.eventId, .at = now}))
      throw std::runtime_error("encounter receipt settle failed");
  }
  purgeSettled(now);
  return EncounterDisposition::Ack;
}

void EncounterClosedConsumer::purgeSettled(int64_t now)
{
  int64_t due = nextPurgeAt_.load(std::memory_order_relaxed);
  if (now < due ||
      !nextPurgeAt_.compare_exchange_strong(due, now + kPurgeIntervalS))
    return;
  int64_t purged = 0;
  {
    std::scoped_lock lock(dependencies_.graph->mutex());
    purged = dependencies_.repository->purgeSettledEncounters(
        dependencies_.graph->handle(),
        {.olderThan = now - kSettledReceiptRetentionS, .limit = kPurgeBatch});
  }
  if (purged >= kPurgeBatch)
    nextPurgeAt_.store(now + kPurgeBacklogIntervalS, std::memory_order_relaxed);
  if (purged > 0)
    LOG_INFO << "Encounter consumer: purged " << purged << " settled receipt(s)";
}

void EncounterClosedConsumer::settleDelivery(const EncounterDelivery& delivery)
{
  const auto& settlement = delivery.settlement;
  EncounterDisposition disposition = EncounterDisposition::Nak;
  if (delivery.alive) {
    try {
      disposition = settlePayload(delivery.payload);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Encounter consumer: redelivering (" << error.what() << ")";
      disposition = EncounterDisposition::Nak;
    }
  }
  if (disposition == EncounterDisposition::Term) {
    if (settlement.term)
      settlement.term();
  }
  else if (disposition == EncounterDisposition::Ack) {
    if (settlement.ack)
      settlement.ack();
  }
  else if (settlement.nak) {
    settlement.nak();
  }
}

bool EncounterClosedConsumer::trySubscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = true,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kDefaultMaxAckPending,
       .handler = [this, lifecycle = lifecycle_](
                         const NatsBus::DurableMessage& message,
                         NatsBus::DurableSettlement settlement) {
         const auto nak = settlement.nak;
         try {
           strand_.post([this, lifecycle, payload = std::string(message.payload),
                         settlement = std::move(settlement)]() {
             const EncounterLifecycleGuard guard(lifecycle);
             settleDelivery({.alive = guard.alive(), .payload = payload, .settlement = settlement});
           });
         }
         catch (const std::exception& error) {
           LOG_WARN << "Encounter consumer: redelivering, not scheduled (" << error.what() << ")";
           if (nak)
             nak();
         }
       }});
  if (!subscription)
    return false;
  subscription_ = *subscription;
  return true;
}

void EncounterClosedConsumer::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(
      5.0, [this, lifecycle = lifecycle_]() {
        const EncounterLifecycleGuard guard(lifecycle);
        if (!guard.alive() || subscription_.has_value() ||
            dependencies_.bus == nullptr)
          return;
        if (trySubscribe())
          LOG_INFO << "Encounter consumer: durable consumer connected";
      });
}
