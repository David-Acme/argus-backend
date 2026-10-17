#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace viewer_slots
{

struct Limits
{
  int perCamera{0};
  int total{0};
};

struct Viewer
{
  int64_t cameraId{0};
  bool recorder{false};
};

struct Occupancy
{
  int perCamera{0};
  int total{0};
};

struct Request
{
  bool recorder{false};
  bool priority{true};
};

enum class Refusal : uint8_t
{
  None = 0,
  Total,
  Camera
};

inline constexpr std::string_view kTooManyViewers = "too_many_viewers";
inline constexpr std::string_view kTooManyViewersForCamera =
    "too_many_viewers_for_camera";

[[nodiscard]] Occupancy occupancyOf(const std::vector<Viewer>& viewers,
                                    int64_t cameraId);
[[nodiscard]] Refusal refusalOf(const Limits& limits,
                                const Occupancy& occupancy,
                                const Request& request);
[[nodiscard]] const char* refusalMessage(Refusal refusal);

}
