#include "member-match.hxx"

#include <algorithm>
#include <ranges>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>

std::optional<MemberMatch> member_match::decide(const MemberMatchInput& input)
{
  std::optional<MemberMatch> best;
  float runnerUp = -1.0F;
  for (const auto& candidate : input.candidates) {
    if (!candidate.member)
      continue;
    if (!best || candidate.score > best->score) {
      if (best)
        runnerUp = std::max(runnerUp, best->score);
      best = MemberMatch{.personId = candidate.personId, .score = candidate.score};
    }
    else {
      runnerUp = std::max(runnerUp, candidate.score);
    }
  }
  if (!best || best->score < input.threshold)
    return std::nullopt;
  if (runnerUp >= 0.0F && best->score - runnerUp < input.margin)
    return std::nullopt;
  return best;
}

drogon::Task<std::optional<MemberMatch>>
MemberMatcher::match(MemberMatchRequest request) const
{
  const auto neighbours = co_await BlockingTask<std::vector<FaceNeighbour>>(
      [embedding = std::move(request.embedding)] {
        return FaceService::instance().faceDb().nearest(
            {.query = embedding.data(), .topK = member_match::kNeighbours});
      });
  if (neighbours.empty())
    co_return std::nullopt;
  std::vector<int64_t> ids;
  ids.reserve(neighbours.size());
  std::ranges::transform(neighbours, std::back_inserter(ids), &FaceNeighbour::personId);
  const auto members = co_await personRepository_.findMemberIds(ids);
  std::vector<MemberCandidate> candidates;
  candidates.reserve(neighbours.size());
  for (const auto& neighbour : neighbours)
    candidates.push_back({.personId = neighbour.personId,
                          .score = neighbour.score,
                          .member = std::ranges::find(members, neighbour.personId) !=
                                    members.end()});
  co_return member_match::decide({.candidates = candidates,
                                  .threshold = request.threshold,
                                  .margin = request.margin});
}
