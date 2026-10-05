#include "response-verdict-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <exception>
#include <string>

struct VerdictFeedState
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
};

namespace
{
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

ResponseVerdictFeed::ResponseVerdictFeed(NatsBus* bus)
    : bus_(bus), state_(std::make_shared<VerdictFeedState>())
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

void ResponseVerdictFeed::start()
{
  if (bus_ == nullptr || subscription_ || !state_->alive.load(std::memory_order_acquire))
    return;
  subscription_ = bus_->subscribe(
      std::string(nats_subject::kNotificationResponseVerdict),
      [this, state = state_](std::string_view, std::string_view payload) {
        if (!state->alive.load(std::memory_order_acquire))
          return;
        const auto review = response_verdict::reviewOf(payload);
        if (!review)
          return;
        state->active.fetch_add(1, std::memory_order_acq_rel);
        drogon::app().getLoop()->queueInLoop([this, state, input = *review]() {
          drogon::async_run([this, state, input]() -> drogon::Task<void> {
            const VerdictScope scope(state);
            if (!state->alive.load(std::memory_order_acquire))
              co_return;
            try {
              if (!co_await repository_.review(input))
                LOG_WARN << "Guard: verdict for episode " << input.encounterId
                         << " found no episode";
            }
            catch (const std::exception& error) {
              LOG_WARN << "Guard: verdict for episode " << input.encounterId
                       << " not recorded: " << error.what();
            }
          });
        });
      });
  if (subscription_)
    LOG_INFO << "Guard listens for response verdicts on "
             << nats_subject::kNotificationResponseVerdict;
}
