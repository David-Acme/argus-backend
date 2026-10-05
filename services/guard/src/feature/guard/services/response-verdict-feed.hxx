#pragma once

#include <feature/guard/repositories/episode/episode-repository.hxx>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

class NatsBus;

namespace response_verdict
{
std::optional<EpisodeReviewInput> reviewOf(std::string_view payload);
}

struct VerdictFeedState;

class ResponseVerdictFeed
{
public:
  explicit ResponseVerdictFeed(NatsBus* bus);
  ~ResponseVerdictFeed();

  ResponseVerdictFeed(const ResponseVerdictFeed&) = delete;
  ResponseVerdictFeed& operator=(const ResponseVerdictFeed&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  NatsBus* bus_;
  std::optional<uint64_t> subscription_;
  EpisodeRepository repository_;
  std::shared_ptr<VerdictFeedState> state_;
};
