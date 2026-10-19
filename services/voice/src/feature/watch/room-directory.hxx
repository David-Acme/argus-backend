#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct CameraPlace
{
  int64_t cameraId{0};
  int64_t environmentId{0};
  std::string room;
  bool outdoor{false};
};

class RoomDirectory
{
public:
  void replace(const std::vector<CameraPlace>& places);

  [[nodiscard]] bool sameEnvironment(int64_t left, int64_t right) const;
  [[nodiscard]] bool sameRoom(int64_t left, int64_t right) const;
  [[nodiscard]] std::string roomOf(int64_t cameraId) const;
  [[nodiscard]] std::vector<int64_t> roommates(int64_t cameraId) const;
  [[nodiscard]] bool known() const;

private:
  [[nodiscard]] std::optional<CameraPlace> placeOf(int64_t cameraId) const;

  mutable std::mutex mutex_;
  std::map<int64_t, CameraPlace> places_;
};
