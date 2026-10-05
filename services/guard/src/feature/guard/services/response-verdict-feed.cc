#include "response-verdict-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <string>

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

ResponseVerdictFeed::ResponseVerdictFeed(NatsBus* bus) : bus_(bus) {}

ResponseVerdictFeed::~ResponseVerdictFeed()
{
  if (bus_ != nullptr && subscription_)
    bus_->unsubscribe(*subscription_);
}

void ResponseVerdictFeed::start()
{
  if (bus_ == nullptr || subscription_)
    return;
  subscription_ = bus_->subscribe(
      std::string(nats_subject::kNotificationResponseVerdict),
      [this](std::string_view, std::string_view payload) {
        const auto review = response_verdict::reviewOf(payload);
        if (!review)
          return;
        drogon::app().getLoop()->queueInLoop([this, input = *review]() {
          drogon::async_run([this, input]() -> drogon::Task<void> {
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
