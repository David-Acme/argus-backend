#include "person-sightings.hxx"

#include <algorithm>

PersonSightings& PersonSightings::instance()
{
  static PersonSightings sightings;
  return sightings;
}

void PersonSightings::configure(int64_t holdMs)
{
  std::scoped_lock lock(mutex_);
  if (holdMs > 0)
    holdMs_ = holdMs;
}

void PersonSightings::note(const PersonSighting& sighting)
{
  std::scoped_lock lock(mutex_);
  auto& list = cameras_[sighting.cameraId];
  list.push_back(sighting);
  while (!list.empty() && (list.size() > kMaxPerCamera || sighting.atMs - list.front().atMs > holdMs_))
    list.pop_front();
}

std::vector<PersonSighting> PersonSightings::recent(const SightingQuery& query) const
{
  std::vector<PersonSighting> out;
  std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(query.cameraId);
  if (found == cameras_.end())
    return out;
  for (const PersonSighting& sighting : found->second)
    if (sighting.atMs >= query.sinceMs)
      out.push_back(sighting);
  return out;
}

std::vector<PersonSighting> PersonSightings::since(int64_t sinceMs) const
{
  std::vector<PersonSighting> out;
  std::scoped_lock lock(mutex_);
  for (const auto& [cameraId, list] : cameras_)
    for (const PersonSighting& sighting : list)
      if (sighting.atMs >= sinceMs)
        out.push_back(sighting);
  std::ranges::sort(out, {}, &PersonSighting::atMs);
  return out;
}

void PersonSightings::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  cameras_.erase(cameraId);
}
