#include "webrtc-admission.hxx"

#include <algorithm>

namespace
{
constexpr auto kReleasedSeatMemory = std::chrono::seconds(10);
constexpr auto kTagMemory = std::chrono::seconds(30);
}

bool WebRtcAdmission::hasRoom(const WebRtcRoomCheck& check)
{
  if (check.limit <= 0)
    return true;
  const int ceiling = check.priority || check.limit <= 1 ? check.limit : check.limit - 1;
  return check.used < ceiling;
}

void WebRtcAdmission::prune(const WebRtcSeatRequest& request)
{
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(seats_, [now](const Seat& seat) {
    return seat.released && now - seat.releasedAt > kReleasedSeatMemory;
  });
  std::erase_if(owners_, [&request, now](const auto& entry) {
    const bool watching = std::ranges::any_of(request.consumers, [&entry](const auto& consumer) {
      return consumer.userAgent == entry.first;
    });
    return !watching && now - entry.second.at > kTagMemory;
  });
}

WebRtcSeatVerdict WebRtcAdmission::reserve(const WebRtcSeatRequest& request)
{
  std::scoped_lock lock(mutex_);
  prune(request);
  int perCamera = request.perCamera;
  int total = request.total;
  int perUser = static_cast<int>(std::ranges::count_if(request.consumers, [this, &request](const auto& consumer) {
    const auto owner = owners_.find(consumer.userAgent);
    return owner != owners_.end() && owner->second.userId == request.userId;
  }));
  for (const Seat& seat : seats_) {
    if (seat.released && seat.releasedAt <= request.snapshotAt)
      continue;
    ++total;
    if (seat.cameraId == request.cameraId)
      ++perCamera;
    if (seat.userId == request.userId)
      ++perUser;
  }
  if (!hasRoom({.used = total, .limit = request.limits.total, .priority = request.priority}))
    return {.ticket = 0, .refusal = WebRtcSeatRefusal::Total};
  if (!hasRoom({.used = perCamera, .limit = request.limits.perCamera, .priority = request.priority}))
    return {.ticket = 0, .refusal = WebRtcSeatRefusal::Camera};
  if (request.limits.perUser > 0 && perUser >= request.limits.perUser)
    return {.ticket = 0, .refusal = WebRtcSeatRefusal::User};
  const uint64_t ticket = nextTicket_++;
  seats_.push_back({.ticket = ticket,
                    .cameraId = request.cameraId,
                    .userId = request.userId,
                    .released = false,
                    .releasedAt = {}});
  return {.ticket = ticket, .refusal = WebRtcSeatRefusal::None};
}

void WebRtcAdmission::release(const WebRtcSeatRelease& seat)
{
  std::scoped_lock lock(mutex_);
  const auto found = std::ranges::find(seats_, seat.ticket, &Seat::ticket);
  if (found == seats_.end())
    return;
  found->released = true;
  found->releasedAt = seat.at;
}

void WebRtcAdmission::remember(const WebRtcTagOwner& owner)
{
  std::scoped_lock lock(mutex_);
  owners_[owner.tag] = {.userId = owner.userId, .at = owner.at};
}

std::vector<std::string> WebRtcAdmission::tagsOf(int64_t userId)
{
  std::scoped_lock lock(mutex_);
  std::vector<std::string> tags;
  for (const auto& [tag, owner] : owners_) {
    if (owner.userId == userId)
      tags.push_back(tag);
  }
  return tags;
}
