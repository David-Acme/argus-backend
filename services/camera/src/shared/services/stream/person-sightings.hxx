#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

struct PersonSighting
{
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t personId{0};
  bool known{false};
  float centerX{0.5F};
  float centerY{0.5F};
  float width{0.0F};
  float height{0.0F};
  bool night{false};
  int64_t atMs{0};
};

struct SightingQuery
{
  int64_t cameraId{0};
  int64_t sinceMs{0};
};

class PersonSightings
{
public:
  static PersonSightings& instance();

  void configure(int64_t holdMs);
  void note(const PersonSighting& sighting);
  [[nodiscard]] std::vector<PersonSighting> recent(const SightingQuery& query) const;
  [[nodiscard]] std::vector<PersonSighting> since(int64_t sinceMs) const;
  void forget(int64_t cameraId);

private:
  PersonSightings() = default;

  static constexpr size_t kMaxPerCamera = 64;

  mutable std::mutex mutex_;
  int64_t holdMs_{45000};
  std::unordered_map<int64_t, std::deque<PersonSighting>> cameras_;
};
