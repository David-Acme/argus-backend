#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/repositories/person/person-repository.hxx>
#include <span>
#include <vector>

struct MemberCandidate
{
  int64_t personId{0};
  float score{0.0F};
  bool member{false};
};

struct MemberMatchInput
{
  std::span<const MemberCandidate> candidates;
  float threshold{0.0F};
  float margin{0.0F};
};

struct MemberMatch
{
  int64_t personId{0};
  float score{0.0F};
};

struct MemberMatchRequest
{
  std::vector<float> embedding;
  float threshold{0.0F};
  float margin{0.0F};
};

namespace member_match
{
inline constexpr int kNeighbours = 24;

[[nodiscard]] std::optional<MemberMatch> decide(const MemberMatchInput& input);
}

class MemberMatcher
{
public:
  [[nodiscard]] drogon::Task<std::optional<MemberMatch>>
  match(MemberMatchRequest request) const;

private:
  PersonRepository personRepository_;
};
