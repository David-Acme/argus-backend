#include "change-feed-consumer.hxx"

#include <drogon/drogon.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <exception>
#include <utility>

namespace change_feed
{
const std::vector<Feed>& defaults()
{
  static const std::vector<Feed> feeds{
      {.stream = nats_subject::kCameraStream,
       .subject = nats_subject::kCameraChange,
       .durable = "argus-sync-camera",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
      {.stream = nats_subject::kNotificationChangeStream,
       .subject = nats_subject::kNotificationChange,
       .durable = "argus-sync-notification",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
      {.stream = nats_subject::kProductivityChangeStream,
       .subject = nats_subject::kProductivityChange,
       .durable = "argus-sync-productivity",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
      {.stream = nats_subject::kIdentityChangeStream,
       .subject = nats_subject::kIdentityChange,
       .durable = "argus-sync-identity",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
      {.stream = nats_subject::kIdentityChangeStream,
       .subject = nats_subject::kIdentityUserAction,
       .durable = "argus-sync-identity-action",
       .maxAckPending = NatsBus::kDefaultMaxAckPending},
      {.stream = nats_subject::kAuthChangeStream,
       .subject = nats_subject::kAuthUserAction,
       .durable = "argus-sync-auth-action",
       .maxAckPending = NatsBus::kDefaultMaxAckPending},
      {.stream = nats_subject::kAuthSessionStream,
       .subject = nats_subject::kAuthSession,
       .durable = "argus-sync-auth-session",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
      {.stream = nats_subject::kSettingsModuleStream,
       .subject = nats_subject::kSettingsModule,
       .durable = "argus-sync-settings-module",
       .maxAckPending = NatsBus::kOrderedMaxAckPending},
  };
  return feeds;
}
}

ChangeFeedConsumer::ChangeFeedConsumer(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)),
      config_(std::move(config))
{
  attachments_.reserve(config_.feeds.size());
  for (const auto& feed : config_.feeds)
    attachments_.push_back(Attachment{.feed = feed, .subscription = {}});
}

ChangeFeedConsumer::~ChangeFeedConsumer()
{
  stop();
}

void ChangeFeedConsumer::start()
{
  if (dependencies_.bus == nullptr || dependencies_.auditFanOut == nullptr)
    return;
  if (subscribePending())
    return;
  LOG_WARN << "Change feed: streams not ready; retrying";
  scheduleSubscribeRetry();
}

void ChangeFeedConsumer::stop()
{
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  for (auto& attachment : attachments_) {
    if (attachment.subscription.has_value() && dependencies_.bus != nullptr)
      dependencies_.bus->unsubscribe(*attachment.subscription);
    attachment.subscription.reset();
  }
}

void ChangeFeedConsumer::requestStop()
{
  stop();
}

bool ChangeFeedConsumer::drained() const
{
  return pending_->load(std::memory_order_acquire) == 0;
}

drogon::Task<DurableDisposition>
ChangeFeedConsumer::handle(const durable_delivery::Payload& message)
{
  const Json::Value json = json_util::fromString(message.body);
  if (!json.isObject()) {
    LOG_WARN << "Change feed: payload on " << message.subject
             << " is not a JSON object; refused";
    co_return DurableDisposition::Term;
  }
  if (message.subject == nats_subject::kSettingsModule)
    co_return sync_fan_out::handleModulePayload(json);
  if (message.subject == nats_subject::kIdentityUserAction ||
      message.subject == nats_subject::kAuthUserAction)
    co_return co_await sync_fan_out::handleActionPayload(
        {.json = json,
         .msgId = message.msgId,
         .auditFanOut = *dependencies_.auditFanOut});
  co_return co_await sync_fan_out::handleChangePayload(
      {.json = json,
       .subject = message.subject,
       .auditFanOut = *dependencies_.auditFanOut});
}

bool ChangeFeedConsumer::trySubscribe(Attachment& attachment)
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = attachment.feed.stream,
       .durable = attachment.feed.durable,
       .subject = attachment.feed.subject,
       .deliverAll = false,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = attachment.feed.maxAckPending,
       .handler = durable_delivery::handler(
           {.label = "Change feed " + attachment.feed.durable,
            .pending = pending_},
           [this](const durable_delivery::Payload& payload) {
             return handle(payload);
           })});
  if (!subscription)
    return false;
  attachment.subscription = subscription;
  return true;
}

bool ChangeFeedConsumer::subscribePending()
{
  bool allAttached = true;
  for (auto& attachment : attachments_) {
    if (attachment.subscription.has_value())
      continue;
    if (trySubscribe(attachment))
      LOG_INFO << "Change feed: durable " << attachment.feed.durable
               << " connected on " << attachment.feed.subject;
    else
      allAttached = false;
  }
  return allAttached;
}

void ChangeFeedConsumer::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(5.0, [this]() {
    if (dependencies_.bus == nullptr || !subscribePending() ||
        !retryTimer_.has_value())
      return;
    drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  });
}
