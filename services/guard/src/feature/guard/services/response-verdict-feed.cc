#include "response-verdict-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <sync/stream-retention.hxx>

#include <atomic>
#include <exception>
#include <string>
#include <utility>

struct VerdictFeedState
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
};

namespace
{
constexpr double kAttachRetryS = 5.0;

class VerdictScope
{
public:
  explicit VerdictScope(std::shared_ptr<VerdictFeedState> state) : state_(std::move(state)) {}
  ~VerdictScope() { state_->active.fetch_sub(1, std::memory_order_acq_rel); }
  VerdictScope(const VerdictScope&) = delete;
  VerdictScope& operator=(const VerdictScope&) = delete;

private:
  std::shared_ptr<VerdictFeedState> state_;
};

struct SettleInput
{
  VerdictSettle outcome{VerdictSettle::Retry};
  const NatsBus::DurableSettlement& settlement;
};

void settle(const SettleInput& input)
{
  const auto call = [](const std::function<void()>& action) {
    if (action)
      action();
  };
  switch (input.outcome) {
    case VerdictSettle::Ack:
      call(input.settlement.ack);
      return;
    case VerdictSettle::Discard:
      call(input.settlement.term);
      return;
    case VerdictSettle::Retry:
      call(input.settlement.nak);
      return;
  }
}
}

std::optional<EpisodeReviewInput> response_verdict::reviewOf(std::string_view payload)
{
  const Json::Value event = json_util::fromString(std::string(payload));
  if (!event.isObject() || event.get("kind", "").asString() != "guard_episode")
    return std::nullopt;
  const int64_t episodeId = event.get("episodeId", 0).asInt64();
  const std::string verdict = event.get("verdict", "").asString();
  if (episodeId <= 0 || (verdict != "real" && verdict != "false_alarm"))
    return std::nullopt;
  return EpisodeReviewInput{.encounterId = episodeId,
                            .label = verdict == "real" ? FeedbackLabel::Useful
                                                       : FeedbackLabel::FalseAlarm,
                            .at = event.get("at", 0).asInt64()};
}

ResponseVerdictFeed::ResponseVerdictFeed(NatsBus* bus, ResponseVerdictFeedConfig config)
    : bus_(bus), config_(std::move(config)), state_(std::make_shared<VerdictFeedState>())
{
}

ResponseVerdictFeed::~ResponseVerdictFeed()
{
  requestStop();
}

void ResponseVerdictFeed::requestStop()
{
  state_->alive.store(false, std::memory_order_release);
  if (bus_ != nullptr && subscription_)
    bus_->unsubscribe(*subscription_);
  subscription_.reset();
}

bool ResponseVerdictFeed::drained() const
{
  return state_->active.load(std::memory_order_acquire) == 0;
}

drogon::Task<VerdictSettle> ResponseVerdictFeed::apply(std::string_view payload) const
{
  const Json::Value event = json_util::fromString(std::string(payload));
  if (!event.isObject())
    co_return VerdictSettle::Discard;
  const auto review = response_verdict::reviewOf(payload);
  if (!review)
    co_return VerdictSettle::Ack;
  if (co_await repository_.review(*review))
    co_return VerdictSettle::Ack;
  if (co_await repository_.find(review->encounterId)) {
    LOG_WARN << "Guard: verdict for episode " << review->encounterId
             << " not stored yet; it will be redelivered";
    co_return VerdictSettle::Retry;
  }
  LOG_WARN << "Guard: verdict for episode " << review->encounterId << " found no episode";
  co_return VerdictSettle::Ack;
}

void ResponseVerdictFeed::start()
{
  if (bus_ == nullptr || subscription_ || !state_->alive.load(std::memory_order_acquire))
    return;
  if (attach())
    return;
  LOG_WARN << "Guard: the response verdict consumer could not be attached; retrying";
  drogon::app().getLoop()->runAfter(kAttachRetryS, [this, state = state_]() {
    if (state->alive.load(std::memory_order_acquire))
      start();
  });
}

bool ResponseVerdictFeed::attach()
{
  subscription_ = bus_->subscribeDurableFeed(
      {.stream = {.name = config_.stream,
                  .subjects = {config_.subject},
                  .maxAgeNs = stream_retention::kRetentionNs,
                  .duplicatesNs = stream_retention::kDuplicatesNs},
       .consumer = {.stream = config_.stream,
                    .durable = config_.durable,
                    .subject = config_.subject,
                    .deliverAll = true,
                    .maxDeliver = config_.maxDeliver,
                    .maxAckPending = NatsBus::kDefaultMaxAckPending,
                    .handler = [this, state = state_](const NatsBus::DurableMessage& message,
                                                      NatsBus::DurableSettlement settlement) {
                      if (!state->alive.load(std::memory_order_acquire)) {
                        if (settlement.nak)
                          settlement.nak();
                        return;
                      }
                      state->active.fetch_add(1, std::memory_order_acq_rel);
                      drogon::app().getLoop()->queueInLoop(
                          [this, state, payload = std::string(message.payload),
                           settlement = std::move(settlement)]() mutable {
                            drogon::async_run(
                                [this, state, payload = std::move(payload),
                                 settlement = std::move(settlement)]() -> drogon::Task<void> {
                                  const VerdictScope scope(state);
                                  VerdictSettle outcome = VerdictSettle::Retry;
                                  if (state->alive.load(std::memory_order_acquire)) {
                                    try {
                                      outcome = co_await apply(payload);
                                    }
                                    catch (const std::exception& error) {
                                      LOG_WARN << "Guard: verdict not recorded: "
                                               << error.what();
                                    }
                                  }
                                  settle({.outcome = outcome, .settlement = settlement});
                                });
                          });
                    }}});
  if (subscription_)
    LOG_INFO << "Guard consumes response verdicts from " << config_.stream << " ("
             << config_.durable << ")";
  return subscription_.has_value();
}
