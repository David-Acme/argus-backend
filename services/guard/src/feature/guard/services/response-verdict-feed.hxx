#pragma once

#include <feature/guard/repositories/episode/episode-repository.hxx>

#include <cstdint>
#include <optional>
#include <string_view>

class NatsBus;

namespace response_verdict
{
std::optional<EpisodeReviewInput> reviewOf(std::string_view payload);
}

class ResponseVerdictFeed
{
public:
  explicit ResponseVerdictFeed(NatsBus* bus);
  ~ResponseVerdictFeed();

  ResponseVerdictFeed(const ResponseVerdictFeed&) = delete;
  ResponseVerdictFeed& operator=(const ResponseVerdictFeed&) = delete;

  void start();

private:
  NatsBus* bus_;
  std::optional<uint64_t> subscription_;
  EpisodeRepository repository_;
};
