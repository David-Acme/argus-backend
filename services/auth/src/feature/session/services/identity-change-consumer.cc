#include "identity-change-consumer.hxx"

#include <drogon/drogon.h>
#include <feature/session/infra/ordered-delivery.hxx>
#include <feature/session/services/session-service.hxx>
#include <json/value.h>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr const char* kConsumerLabel = "Identity change";
constexpr const char* kIsActiveField = "isActive";
constexpr double kSubscribeRetrySeconds = 5.0;

int64_t recordId(const Json::Value& event)
{
  const Json::Value& id = event[sync_change::kRecordIdField];
  return id.isIntegral() ? id.asInt64() : 0;
}

class InFlightCounter
{
public:
  explicit InFlightCounter(std::atomic<int64_t>& counter)
      : counter_(counter)
  {
    counter_.fetch_add(1, std::memory_order_acq_rel);
  }

  ~InFlightCounter() { counter_.fetch_sub(1, std::memory_order_acq_rel); }

  InFlightCounter(const InFlightCounter&) = delete;
  InFlightCounter& operator=(const InFlightCounter&) = delete;

private:
  std::atomic<int64_t>& counter_;
};
}

IdentityChangeConsumer::IdentityChangeConsumer(Dependencies dependencies,
                                               Config config)
    : dependencies_(dependencies), config_(std::move(config))
{
}

IdentityChangeConsumer::~IdentityChangeConsumer()
{
  stop();
}

void IdentityChangeConsumer::start()
{
  if (dependencies_.bus == nullptr || dependencies_.sessions == nullptr)
    return;
  if (subscribe())
    return;
  LOG_WARN << kConsumerLabel << ": stream not ready; retrying";
  scheduleSubscribeRetry();
}

void IdentityChangeConsumer::stop()
{
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  if (subscription_.has_value() && dependencies_.bus != nullptr)
    dependencies_.bus->unsubscribe(*subscription_);
  subscription_.reset();
}

void IdentityChangeConsumer::requestStop()
{
  stop();
}

bool IdentityChangeConsumer::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}

bool IdentityChangeConsumer::subscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = false,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kOrderedMaxAckPending,
       .handler = ordered_delivery::handler(
           kConsumerLabel,
           [this](const std::string& body) { return handle(body); })});
  if (!subscription)
    return false;
  subscription_ = subscription;
  LOG_INFO << kConsumerLabel << ": durable " << config_.durable
           << " connected on " << config_.subject;
  return true;
}

void IdentityChangeConsumer::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(
      kSubscribeRetrySeconds, [this]() {
        if (!subscribe() || !retryTimer_.has_value())
          return;
        drogon::app().getLoop()->invalidateTimer(*retryTimer_);
        retryTimer_.reset();
      });
}

drogon::Task<void> IdentityChangeConsumer::handle(const std::string& body)
{
  const InFlightCounter inFlight(inFlight_);
  const Json::Value event = json_util::fromString(body);
  if (event.get(sync_change::kKindField, "").asString() !=
          sync_change::kKindIdentity ||
      event.get(sync_change::kTableField, "").asString() !=
          tableNameToString(TableName::User))
    co_return;

  const int64_t userId = recordId(event);
  if (userId <= 0) {
    LOG_WARN << kConsumerLabel << ": a user change carried no record id";
    co_return;
  }

  dependencies_.sessions->forget(userId);

  const Json::Value& row = event[sync_change::kRowField];
  if (!row.isObject() || row.get(kIsActiveField, true).asBool())
    co_return;

  if (!co_await dependencies_.sessions->revokeUser(userId))
    LOG_DEBUG << kConsumerLabel << ": user " << userId
              << " was disabled with no live session left to revoke";
  co_return;
}
