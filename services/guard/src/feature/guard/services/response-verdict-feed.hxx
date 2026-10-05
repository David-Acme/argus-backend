#pragma once

#include <feature/guard/repositories/episode/episode-repository.hxx>

#include <nats/nats-subject.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class NatsBus;

namespace response_verdict
{
std::optional<EpisodeReviewInput> reviewOf(std::string_view payload);
}

enum class VerdictSettle : uint8_t
{
  Ack,
  Retry,
  Discard
};

struct ResponseVerdictFeedConfig
{
  std::string stream{nats_subject::kNotificationVerdictStream};
  std::string subject{nats_subject::kNotificationResponseVerdict};
  std::string durable{"argus-guard-verdicts"};
  int maxDeliver{20};
};

struct VerdictFeedState;

class ResponseVerdictFeed
{
public:
  explicit ResponseVerdictFeed(NatsBus* bus, ResponseVerdictFeedConfig config = {});
  ~ResponseVerdictFeed();

  ResponseVerdictFeed(const ResponseVerdictFeed&) = delete;
  ResponseVerdictFeed& operator=(const ResponseVerdictFeed&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  [[nodiscard]] drogon::Task<VerdictSettle> apply(std::string_view payload) const;

private:
  bool attach();

  NatsBus* bus_;
  ResponseVerdictFeedConfig config_;
  std::optional<uint64_t> subscription_;
  EpisodeRepository repository_;
  std::shared_ptr<VerdictFeedState> state_;
};
