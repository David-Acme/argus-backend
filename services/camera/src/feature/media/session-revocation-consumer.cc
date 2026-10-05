#include "session-revocation-consumer.hxx"

#include <drogon/drogon.h>
#include <nats/nats-subject.hxx>
#include <shared/utils/in-flight/in-flight.hxx>
#include <sync/sync-change.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <utility>

namespace
{
constexpr double kSubscribeRetrySeconds = 5.0;

void settleWith(const std::function<void()>& outcome)
{
  if (outcome)
    outcome();
}

bool namesSessionAction(const std::string& body)
{
  const Json::Value change = json_util::fromString(body);
  return change.isObject() &&
         change.get(sync_change::kActionField, "").asString() ==
             sync_change::kActionDisconnectSession;
}
}

SessionRevocationConsumer::Config SessionRevocationConsumer::defaults()
{
  return {.stream = nats_subject::kAuthSessionStream,
          .subject = nats_subject::kAuthSession,
          .durable = "argus-camera-auth-session",
          .maxDeliver = 10};
}

SessionRevocationConsumer::SessionRevocationConsumer(Dependencies dependencies,
                                                     Config config)
    : dependencies_(std::move(dependencies)), config_(std::move(config))
{
}

SessionRevocationConsumer::~SessionRevocationConsumer()
{
  requestStop();
  if (connector_.joinable())
    connector_.join();
}

void SessionRevocationConsumer::start()
{
  if (!dependencies_.bus || dependencies_.sessions == nullptr || connector_.joinable())
    return;
  connecting_.store(true, std::memory_order_release);
  connector_ = std::jthread([this](const std::stop_token& stop) { connect(stop); });
}

void SessionRevocationConsumer::connect(const std::stop_token& stop)
{
  bool warned = false;
  while (!stop.stop_requested()) {
    if (subscribe()) {
      if (stop.stop_requested())
        requestStop();
      break;
    }
    if (!warned)
      LOG_WARN << "Media session revocations: stream not ready; retrying";
    warned = true;
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, std::chrono::duration<double>(kSubscribeRetrySeconds),
                   [] { return false; });
  }
  connecting_.store(false, std::memory_order_release);
}

void SessionRevocationConsumer::requestStop()
{
  connector_.request_stop();
  std::optional<uint64_t> subscription;
  {
    std::scoped_lock lock(mutex_);
    subscription.swap(subscription_);
  }
  if (subscription.has_value() && dependencies_.bus)
    dependencies_.bus->unsubscribe(*subscription);
}

bool SessionRevocationConsumer::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0 &&
         !connecting_.load(std::memory_order_acquire);
}

bool SessionRevocationConsumer::subscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = false,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kOrderedMaxAckPending,
       .handler = [this](const NatsBus::DurableMessage& message,
                         const NatsBus::DurableSettlement& settlement) {
         handle(message, settlement);
       }});
  if (!subscription)
    return false;
  {
    std::scoped_lock lock(mutex_);
    subscription_ = subscription;
  }
  LOG_INFO << "Media session revocations: durable " << config_.durable
           << " connected on " << config_.subject;
  return true;
}

void SessionRevocationConsumer::handle(const NatsBus::DurableMessage& message,
                                       const NatsBus::DurableSettlement& settlement)
{
  const in_flight::Guard guard(inFlight_);
  const std::string body(message.payload);
  const auto session = session_revocation::parse(body);
  if (!session) {
    settleWith(namesSessionAction(body) ? settlement.term : settlement.ack);
    return;
  }
  const std::size_t closed = dependencies_.sessions->closeSession(*session);
  if (dependencies_.onRevoked)
    dependencies_.onRevoked(*session);
  if (closed > 0)
    LOG_INFO << "Media session revocations: closed " << closed
             << " live view(s) of a revoked session of user " << session->userId;
  settleWith(settlement.ack);
}
