#include "change-feed-consumer.hxx"

#include <drogon/drogon.h>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace change_feed
{
const std::vector<Feed>& defaults()
{
  static const std::vector<Feed> feeds{
      {.stream = nats_subject::kCameraStream,
       .subject = nats_subject::kCameraChange,
       .durable = "argus-sync-camera"},
      {.stream = nats_subject::kNotificationChangeStream,
       .subject = nats_subject::kNotificationChange,
       .durable = "argus-sync-notification"},
      {.stream = nats_subject::kProductivityChangeStream,
       .subject = nats_subject::kProductivityChange,
       .durable = "argus-sync-productivity"},
      {.stream = nats_subject::kIdentityChangeStream,
       .subject = nats_subject::kIdentityChange,
       .durable = "argus-sync-identity"},
      {.stream = nats_subject::kIdentityChangeStream,
       .subject = nats_subject::kIdentityUserAction,
       .durable = "argus-sync-identity-action"},
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

drogon::Task<DurableDisposition>
ChangeFeedConsumer::handle(const durable_delivery::Payload& message)
{
  const Json::Value json = json_util::fromString(message.body);
  if (message.subject == nats_subject::kIdentityUserAction)
    co_return co_await sync_fan_out::handleActionPayload(
        {.json = json,
         .msgId = message.msgId,
         .auditFanOut = *dependencies_.auditFanOut});
  co_return co_await sync_fan_out::handleChangePayload(
      json, *dependencies_.auditFanOut);
}

bool ChangeFeedConsumer::trySubscribe(Attachment& attachment)
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = attachment.feed.stream,
       .durable = attachment.feed.durable,
       .subject = attachment.feed.subject,
       .deliverAll = false,
       .maxDeliver = config_.maxDeliver,
       .handler = durable_delivery::handler(
           "Change feed " + attachment.feed.durable,
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
