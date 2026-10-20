#include "speaking-camera.hxx"

#include <algorithm>
#include <vector>

namespace
{

double confidenceOf(std::span<const RoomConfidence> rooms, const std::string& room)
{
  double best = 0.0;
  for (const RoomConfidence& entry : rooms)
    if (entry.room == room)
      best = std::max(best, entry.confidence);
  return best;
}

std::optional<size_t> bestIn(std::span<const SpeakingCandidate> candidates, const std::string* room)
{
  std::optional<size_t> best;
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (room != nullptr && candidates[index].room != *room)
      continue;
    if (!best || candidates[index].score > candidates[*best].score)
      best = index;
  }
  return best;
}

}

std::optional<SpeakingChoice> chooseSpeakingCamera(const SpeakingChoiceInput& input)
{
  const std::optional<size_t> arbitrated = bestIn(input.candidates, nullptr);
  if (!arbitrated)
    return std::nullopt;
  const SpeakingChoice fallback{.index = *arbitrated, .byBelief = false, .room = input.candidates[*arbitrated].room};
  std::vector<RoomConfidence> rooms;
  for (const SpeakingCandidate& candidate : input.candidates) {
    if (candidate.room.empty() || std::ranges::find(rooms, candidate.room, &RoomConfidence::room) != rooms.end())
      continue;
    rooms.push_back({.room = candidate.room, .confidence = confidenceOf(input.rooms, candidate.room)});
  }
  if (rooms.size() < 2)
    return fallback;
  std::ranges::stable_sort(rooms, std::ranges::greater{}, &RoomConfidence::confidence);
  const RoomConfidence& first = rooms[0];
  const RoomConfidence& second = rooms[1];
  if (first.confidence < input.minConfidence || first.confidence - second.confidence < input.margin)
    return fallback;
  const std::optional<size_t> chosen = bestIn(input.candidates, &first.room);
  if (!chosen)
    return fallback;
  return SpeakingChoice{.index = *chosen, .byBelief = true, .room = first.room};
}
