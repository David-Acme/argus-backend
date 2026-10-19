#include "relay-recipient.hxx"

#include <text/name-match.hxx>

#include <cstddef>
#include <string>
#include <utility>

namespace relay_recipient
{

Resolution resolve(const ResolveInput& input)
{
  std::vector<std::string> names;
  names.reserve(input.roster.size());
  for (const auto& member : input.roster)
    names.push_back(member.name);

  const text_norm::NameMatch match = text_norm::matchName(names, std::string(input.asked));
  switch (match.kind) {
  case text_norm::NameMatchKind::Exact: {
    const int64_t userId = input.roster[match.hits.front()].userId;
    if (userId == input.senderId)
      return {.kind = ResolutionKind::Self, .userId = userId, .candidates = {}};
    return {.kind = ResolutionKind::Resolved, .userId = userId, .candidates = {}};
  }
  case text_norm::NameMatchKind::Ambiguous: {
    std::vector<int64_t> candidates;
    candidates.reserve(match.hits.size());
    for (const std::size_t index : match.hits)
      candidates.push_back(input.roster[index].userId);
    return {.kind = ResolutionKind::Ambiguous, .userId = 0, .candidates = std::move(candidates)};
  }
  case text_norm::NameMatchKind::Missing:
    break;
  }
  return {.kind = ResolutionKind::Missing, .userId = 0, .candidates = {}};
}

}
