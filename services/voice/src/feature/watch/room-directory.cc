#include "room-directory.hxx"

void RoomDirectory::replace(const std::vector<CameraPlace>& places)
{
  std::map<int64_t, CameraPlace> next;
  for (const CameraPlace& place : places)
    next[place.cameraId] = place;
  std::scoped_lock lock(mutex_);
  places_ = std::move(next);
}

std::optional<CameraPlace> RoomDirectory::placeOf(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = places_.find(cameraId);
  if (found == places_.end())
    return std::nullopt;
  return found->second;
}

bool RoomDirectory::known() const
{
  std::scoped_lock lock(mutex_);
  return !places_.empty();
}

bool RoomDirectory::sameEnvironment(int64_t left, int64_t right) const
{
  if (left == right)
    return true;
  const auto a = placeOf(left);
  const auto b = placeOf(right);
  if (!a || !b)
    return true;
  return a->environmentId == b->environmentId;
}

bool RoomDirectory::sameRoom(int64_t left, int64_t right) const
{
  if (left == right)
    return true;
  const auto a = placeOf(left);
  const auto b = placeOf(right);
  if (!a || !b)
    return false;
  return !a->room.empty() && a->room == b->room;
}

std::string RoomDirectory::roomOf(int64_t cameraId) const
{
  if (const auto place = placeOf(cameraId); place && !place->room.empty())
    return place->room;
  return "camera:" + std::to_string(cameraId);
}

std::vector<int64_t> RoomDirectory::roommates(int64_t cameraId) const
{
  std::vector<int64_t> out;
  const auto self = placeOf(cameraId);
  if (!self || self->room.empty())
    return out;
  std::scoped_lock lock(mutex_);
  for (const auto& [id, place] : places_)
    if (id != cameraId && place.room == self->room)
      out.push_back(id);
  return out;
}
